// Renderer implementation: scene packing, BVH orchestration, dispatch.

#include "Renderer.hpp"
#include "../accel/Bvh.hpp"
#include "../post/Denoiser.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <future>
#include <limits>
#include <map>

// Embedded SPIR-V (generated at build time by cmake/ShaderTools.cmake).
#include "pathtrace_spv.h"
#include "resolve_spv.h"

namespace mrt {

namespace {

// Dev iteration: optionally load fresh SPIR-V from the build dir instead of
// the words embedded at compile time (PRD §13.2).
std::vector<uint32_t> loadSpv(const char* name, const uint32_t* embedded, size_t count) {
#ifdef MRT_DEV_SHADER_RELOAD
    const std::string path = std::string(MRT_SHADER_DIR) + "/" + name + ".spv";
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (f) {
        const size_t bytes = static_cast<size_t>(f.tellg());
        std::vector<uint32_t> words(bytes / 4);
        f.seekg(0);
        f.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(bytes));
        log(LogLevel::Info, std::string("Hot-loaded shader ") + path);
        return words;
    }
    log(LogLevel::Warn, std::string("Shader reload failed, using embedded: ") + path);
#endif
    (void)name;
    return { embedded, embedded + count };
}

constexpr VkBufferUsageFlags kSceneBufUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

// A node whose inverted AABB fails every slab test: traversal of an empty
// scene terminates immediately without touching other buffers.
BvhNode sentinelNode() {
    BvhNode n{};
    n.lo[0] = n.lo[1] = n.lo[2] = std::numeric_limits<float>::max();
    n.hi[0] = n.hi[1] = n.hi[2] = -std::numeric_limits<float>::max();
    n.leftOrPrim = 0;
    n.packed = (1u << 2); // leaf, 1 prim — unreachable behind the inverted AABB
    return n;
}

} // namespace

Renderer::Renderer(GpuContext& gpu, Denoiser* denoiser)
    : m_gpu(gpu), m_denoiser(denoiser) {}

Renderer::~Renderer() {
    VkDevice dev = m_gpu.device();
    if (!dev) return;
    vkDeviceWaitIdle(dev);
    if (m_pathtracePipe) vkDestroyPipeline(dev, m_pathtracePipe, nullptr);
    if (m_resolvePipe)   vkDestroyPipeline(dev, m_resolvePipe, nullptr);
    if (m_pipeLayout)    vkDestroyPipelineLayout(dev, m_pipeLayout, nullptr);
    if (m_setLayout)     vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    if (m_descPool)      vkDestroyDescriptorPool(dev, m_descPool, nullptr);
}

Result Renderer::init(const RenderSettings& settings) {
    m_settings = settings;
    createPipelines();

    // Minimal valid placeholders so every descriptor can be written before
    // the first scene commit.
    auto ensureMin = [&](Buffer& b, VkDeviceSize bytes) { b.create(m_gpu, bytes, kSceneBufUsage, false); };
    // m_nodesBuf must fit the sentinel BvhNode written into it just below;
    // a flat 16-byte placeholder here was a real (if rare) OOB vkCmdCopyBuffer
    // (32 bytes into a 16-byte buffer) that could corrupt GPU allocator state.
    ensureMin(m_nodesBuf, sizeof(BvhNode)); ensureMin(m_trianglesBuf, 16); ensureMin(m_positionsBuf, 16);
    ensureMin(m_normalsBuf, 16); ensureMin(m_uvsBuf, 16); ensureMin(m_tangentsBuf, 16);
    ensureMin(m_instancesBuf, 16); ensureMin(m_materialsBuf, 16); ensureMin(m_lightsBuf, 16);
    ensureMin(m_envCdfBuf, 16);

    m_globalsBuf.create(m_gpu, sizeof(GpuGlobals),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, /*hostVisible*/ true);

    // Pre-commit frames must traverse safely: park a sentinel at the TLAS root.
    const BvhNode sentinel = sentinelNode();
    m_gpu.uploadToBuffer(m_nodesBuf, &sentinel, sizeof(sentinel));
    m_tlasRoot = 0;

    // 1x1 white dummy fills unused texture slots; 1x1 black dummy environment.
    const uint32_t white = 0xFFFFFFFFu;
    m_dummyTexture.create(m_gpu, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    m_gpu.uploadToImage(m_dummyTexture, &white, 4, false);

    const float black[4] = { 0, 0, 0, 1 };
    m_envImage.create(m_gpu, 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    m_gpu.uploadToImage(m_envImage, black, 16, false);

    createFrameResources();
    updateDescriptors();
    return Result::Success;
}

Result Renderer::setSettings(const RenderSettings& s) {
    const bool resize = (s.width != m_settings.width) || (s.height != m_settings.height);
    const bool reset  = resize || (s.maxBounces != m_settings.maxBounces) ||
                        (s.fireflyClamp != m_settings.fireflyClamp);
    m_settings = s;
    if (resize) {
        vkDeviceWaitIdle(m_gpu.device());
        createFrameResources();
        updateDescriptors();
    }
    if (reset) resetAccumulation();
    return Result::Success;
}

void Renderer::createFrameResources() {
    const VkDeviceSize pixels = VkDeviceSize(m_settings.width) * m_settings.height;
    const VkDeviceSize vec4Bytes = pixels * 16;

    m_accumBuf.create(m_gpu, vec4Bytes, kSceneBufUsage, false);
    m_aovAlbedoBuf.create(m_gpu, vec4Bytes, kSceneBufUsage, false);
    m_aovNormalBuf.create(m_gpu, vec4Bytes, kSceneBufUsage, false);
    m_denoisedBuf.create(m_gpu, vec4Bytes, kSceneBufUsage, false);
    m_readbackBuf.create(m_gpu, vec4Bytes,
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         /*hostVisible*/ true);
    m_outputImage.create(m_gpu, m_settings.width, m_settings.height, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    resetAccumulation();
}

// ------------------------------------------------------------- pipelines --
void Renderer::createPipelines() {
    VkDevice dev = m_gpu.device();

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    auto add = [&](uint32_t binding, VkDescriptorType type, uint32_t count = 1) {
        bindings.push_back({ binding, type, count, VK_SHADER_STAGE_COMPUTE_BIT, nullptr });
    };
    for (uint32_t b = BindGlobals; b <= BindEnvCdf; ++b)
        add(b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    add(BindEnvMap,       VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    add(BindTextureArray, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures);
    add(BindOutputImage,  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);

    VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.bindingCount = static_cast<uint32_t>(bindings.size());
    lci.pBindings = bindings.data();
    MRT_VK_CHECK(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &m_setLayout));

    // Tile row offset for pathtrace (PRD §8 A6): a push constant rather than
    // vkCmdDispatchBase, which produced wrong output for non-zero bases in
    // practice despite being used per spec (VK_PIPELINE_CREATE_DISPATCH_BASE_BIT
    // set, no validation errors) — not reliable enough to depend on. Every
    // pipeline sharing this layout must declare a compatible push constant
    // range even if unused, so resolve (which is always dispatched whole,
    // never tiled) gets the same range.
    VkPushConstantRange pushConstant{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) };

    VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pushConstant;
    MRT_VK_CHECK(vkCreatePipelineLayout(dev, &pli, nullptr, &m_pipeLayout));

    m_pathtracePipe = createComputePipeline(g_pathtrace_spv, g_pathtrace_spv_count, "pathtrace");
    m_resolvePipe   = createComputePipeline(g_resolve_spv,   g_resolve_spv_count,   "resolve");

    VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, BindEnvCdf - BindGlobals + 1 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures + 1 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
    };
    VkDescriptorPoolCreateInfo dpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpi.maxSets = 1;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = sizes;
    MRT_VK_CHECK(vkCreateDescriptorPool(dev, &dpi, nullptr, &m_descPool));

    VkDescriptorSetAllocateInfo dai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = m_descPool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &m_setLayout;
    MRT_VK_CHECK(vkAllocateDescriptorSets(dev, &dai, &m_descSet));
}

VkPipeline Renderer::createComputePipeline(const uint32_t* spv, size_t words, const char* name) {
    const std::vector<uint32_t> code = loadSpv(name, spv, words);
    VkShaderModule mod = m_gpu.createShaderModule(code);

    VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = mod;
    ci.stage.pName = "main";
    ci.layout = m_pipeLayout;

    VkPipeline pipe = VK_NULL_HANDLE;
    MRT_VK_CHECK(vkCreateComputePipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &pipe));
    vkDestroyShaderModule(m_gpu.device(), mod, nullptr);
    return pipe;
}

void Renderer::updateDescriptors() {
    VkDevice dev = m_gpu.device();
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> bufInfos;
    bufInfos.reserve(16);

    auto writeBuf = [&](uint32_t binding, const Buffer& b) {
        bufInfos.push_back({ b.handle(), 0, VK_WHOLE_SIZE });
        VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = m_descSet;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bufInfos.back();
        writes.push_back(w);
    };

    writeBuf(BindGlobals,   m_globalsBuf);
    writeBuf(BindAccum,     m_accumBuf);
    writeBuf(BindAovAlbedo, m_aovAlbedoBuf);
    writeBuf(BindAovNormal, m_aovNormalBuf);
    writeBuf(BindDenoised,  m_denoisedBuf);
    writeBuf(BindNodes,     m_nodesBuf);
    writeBuf(BindTriangles, m_trianglesBuf);
    writeBuf(BindPositions, m_positionsBuf);
    writeBuf(BindNormals,   m_normalsBuf);
    writeBuf(BindUvs,       m_uvsBuf);
    writeBuf(BindTangents,  m_tangentsBuf);
    writeBuf(BindInstances, m_instancesBuf);
    writeBuf(BindMaterials, m_materialsBuf);
    writeBuf(BindLights,    m_lightsBuf);
    writeBuf(BindEnvCdf,    m_envCdfBuf);

    VkDescriptorImageInfo envInfo{ m_gpu.linearSampler(), m_envImage.view(),
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet envWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    envWrite.dstSet = m_descSet;
    envWrite.dstBinding = BindEnvMap;
    envWrite.descriptorCount = 1;
    envWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    envWrite.pImageInfo = &envInfo;
    writes.push_back(envWrite);

    std::vector<VkDescriptorImageInfo> texInfos(kMaxTextures);
    for (uint32_t i = 0; i < kMaxTextures; ++i) {
        const Image& img = (i < m_textures.size() && m_textures[i].valid())
                               ? m_textures[i] : m_dummyTexture;
        texInfos[i] = { m_gpu.linearSampler(), img.view(),
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    }
    VkWriteDescriptorSet texWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    texWrite.dstSet = m_descSet;
    texWrite.dstBinding = BindTextureArray;
    texWrite.descriptorCount = kMaxTextures;
    texWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    texWrite.pImageInfo = texInfos.data();
    writes.push_back(texWrite);

    VkDescriptorImageInfo outInfo{ VK_NULL_HANDLE, m_outputImage.view(), VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet outWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    outWrite.dstSet = m_descSet;
    outWrite.dstBinding = BindOutputImage;
    outWrite.descriptorCount = 1;
    outWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    outWrite.pImageInfo = &outInfo;
    writes.push_back(outWrite);

    vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    m_descriptorsDirty = false;
}

// ---------------------------------------------------------- scene packing --
Result Renderer::syncScene(Scene& scene, CommitStats* outStats) {
    const auto t0 = std::chrono::high_resolution_clock::now();
    const uint32_t dirty = scene.dirty();
    if (dirty == DirtyNone) {
        if (outStats) *outStats = CommitStats{};
        return Result::Success;
    }

    vkDeviceWaitIdle(m_gpu.device()); // v1: one frame in flight, safe to repack

    uint32_t blasRebuilt = 0;
    if (dirty & DirtyTextures)  uploadTextures(scene);
    if (dirty & DirtyMaterials) packMaterials(scene);
    if (dirty & DirtyGeometry)  blasRebuilt = packGeometry(scene);
    const bool tlasRebuilt = (dirty & (DirtyGeometry | DirtyInstances | DirtyMaterials)) != 0;
    if (tlasRebuilt)            packInstances(scene);
    if (dirty & DirtyLights)    packLights(scene);
    if (dirty & DirtyEnv)       packEnvironment(scene);

    if (m_descriptorsDirty) updateDescriptors();
    scene.clearDirty();
    resetAccumulation();

    if (outStats) {
        const auto t1 = std::chrono::high_resolution_clock::now();
        outStats->blasRebuilt = blasRebuilt;
        outStats->tlasRebuilt = tlasRebuilt;
        outStats->commitMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
    }
    return Result::Success;
}

namespace {
// Recreate a device buffer if too small (1.5x growth). Returns true if recreated.
bool ensureBuffer(GpuContext& gpu, Buffer& b, VkDeviceSize size) {
    size = std::max<VkDeviceSize>(size, 16);
    if (b.valid() && b.size() >= size) return false;
    const VkDeviceSize cap = std::max<VkDeviceSize>(size, b.size() * 3 / 2);
    b.create(gpu, cap, kSceneBufUsage, false);
    return true;
}
} // namespace

uint32_t Renderer::packGeometry(Scene& scene) {
    // Sorted iteration keeps packing deterministic across commits.
    std::map<MeshId, MeshData*> sorted;
    for (auto& [id, mesh] : scene.meshes()) sorted[id] = &mesh;

    // Drop BLAS entries for removed meshes.
    for (auto it = m_blas.begin(); it != m_blas.end();)
        it = scene.meshes().count(it->first) ? std::next(it) : m_blas.erase(it);

    // Rebuild dirty BLASes concurrently (PRD §4.1).
    std::vector<std::pair<MeshId, std::future<BvhBuildResult>>> jobs;
    for (auto& [id, mesh] : sorted) {
        if (!mesh->blasDirty && m_blas.count(id)) continue;
        MeshData* m = mesh;
        jobs.emplace_back(id, std::async(std::launch::async, [m] {
            const size_t triCount = m->indices.size() / 3;
            std::vector<Aabb> bounds(triCount);
            for (size_t t = 0; t < triCount; ++t) {
                Aabb b;
                b.expand(vec3(m->positions[m->indices[t * 3 + 0]]));
                b.expand(vec3(m->positions[m->indices[t * 3 + 1]]));
                b.expand(vec3(m->positions[m->indices[t * 3 + 2]]));
                bounds[t] = b;
            }
            return BvhBuilder::build(bounds, 4);
        }));
    }
    for (auto& [id, fut] : jobs) {
        BvhBuildResult r = fut.get();
        BlasEntry e;
        e.nodes = std::move(r.nodes);
        e.triOrder = std::move(r.primOrder);
        e.bounds = r.rootBounds;
        m_blas[id] = std::move(e);
        sorted[id]->blasDirty = false;
    }

    // ---- repack global arrays --------------------------------------------
    size_t totalVerts = 0, totalTris = 0, totalNodes = 0;
    for (auto& [id, mesh] : sorted) {
        totalVerts += mesh->positions.size();
        totalTris  += mesh->indices.size() / 3;
        totalNodes += m_blas[id].nodes.size();
    }

    std::vector<vec4>        positions; positions.reserve(totalVerts);
    std::vector<vec4>        normals;   normals.reserve(totalVerts);
    std::vector<vec2>        uvs;       uvs.reserve(totalVerts);
    std::vector<vec4>        tangents;  tangents.reserve(totalVerts);
    std::vector<GpuTriangle> triangles; triangles.reserve(totalTris);
    std::vector<BvhNode>     nodes;     nodes.reserve(totalNodes);

    m_meshVertexBase.clear();
    for (auto& [id, meshPtr] : sorted) {
        MeshData& mesh = *meshPtr;
        BlasEntry& blas = m_blas[id];

        const uint32_t vertexBase = static_cast<uint32_t>(positions.size());
        const uint32_t triBase    = static_cast<uint32_t>(triangles.size());
        const uint32_t nodeBase   = static_cast<uint32_t>(nodes.size());
        m_meshVertexBase[id] = vertexBase;
        blas.rootIndexGlobal = nodeBase;

        positions.insert(positions.end(), mesh.positions.begin(), mesh.positions.end());
        normals.insert(normals.end(), mesh.normals.begin(), mesh.normals.end());
        if (mesh.uvs.empty()) uvs.insert(uvs.end(), mesh.positions.size(), vec2(0));
        else                  uvs.insert(uvs.end(), mesh.uvs.begin(), mesh.uvs.end());
        if (mesh.tangents.empty()) tangents.insert(tangents.end(), mesh.positions.size(), vec4(1, 0, 0, 1));
        else                       tangents.insert(tangents.end(), mesh.tangents.begin(), mesh.tangents.end());

        // Triangles written in BLAS leaf order so leaves address a contiguous range.
        for (uint32_t localTri : blas.triOrder) {
            GpuTriangle t;
            t.i0 = mesh.indices[localTri * 3 + 0] + vertexBase;
            t.i1 = mesh.indices[localTri * 3 + 1] + vertexBase;
            t.i2 = mesh.indices[localTri * 3 + 2] + vertexBase;
            t._pad = 0;
            triangles.push_back(t);
        }

        // Nodes rebased into the global array; leaf prim offsets into the
        // global triangle array.
        for (BvhNode n : blas.nodes) {
            const uint32_t count = n.packed >> 2;
            n.leftOrPrim += (count == 0) ? nodeBase : triBase;
            nodes.push_back(n);
        }
    }

    // TLAS region follows the BLAS nodes; capacity managed in packInstances.
    m_tlasRoot = static_cast<uint32_t>(nodes.size());

    const size_t instCount = scene.instances().size();
    const size_t tlasCapacity = std::max<size_t>(2 * instCount + 1, 4);
    const VkDeviceSize nodeBytes = (nodes.size() + tlasCapacity) * sizeof(BvhNode);

    bool recreated = false;
    recreated |= ensureBuffer(m_gpu, m_nodesBuf, nodeBytes);
    recreated |= ensureBuffer(m_gpu, m_trianglesBuf, triangles.size() * sizeof(GpuTriangle));
    recreated |= ensureBuffer(m_gpu, m_positionsBuf, positions.size() * sizeof(vec4));
    recreated |= ensureBuffer(m_gpu, m_normalsBuf, normals.size() * sizeof(vec4));
    recreated |= ensureBuffer(m_gpu, m_uvsBuf, uvs.size() * sizeof(vec2));
    recreated |= ensureBuffer(m_gpu, m_tangentsBuf, tangents.size() * sizeof(vec4));
    m_descriptorsDirty |= recreated;

    if (!nodes.empty())     m_gpu.uploadToBuffer(m_nodesBuf, nodes.data(), nodes.size() * sizeof(BvhNode));
    if (!triangles.empty()) m_gpu.uploadToBuffer(m_trianglesBuf, triangles.data(), triangles.size() * sizeof(GpuTriangle));
    if (!positions.empty()) m_gpu.uploadToBuffer(m_positionsBuf, positions.data(), positions.size() * sizeof(vec4));
    if (!normals.empty())   m_gpu.uploadToBuffer(m_normalsBuf, normals.data(), normals.size() * sizeof(vec4));
    if (!uvs.empty())       m_gpu.uploadToBuffer(m_uvsBuf, uvs.data(), uvs.size() * sizeof(vec2));
    if (!tangents.empty())  m_gpu.uploadToBuffer(m_tangentsBuf, tangents.data(), tangents.size() * sizeof(vec4));

    return static_cast<uint32_t>(jobs.size());
}

void Renderer::packInstances(Scene& scene) {
    std::map<InstanceId, const InstanceData*> sorted;
    for (auto& [id, inst] : scene.instances()) sorted[id] = &inst;

    // World bounds per instance for the TLAS.
    std::vector<const InstanceData*> flat;
    std::vector<Aabb> bounds;
    flat.reserve(sorted.size());
    bounds.reserve(sorted.size());
    for (auto& [id, inst] : sorted) {
        auto blasIt = m_blas.find(inst->mesh);
        if (blasIt == m_blas.end() || !blasIt->second.bounds.valid()) continue;
        flat.push_back(inst);
        bounds.push_back(blasIt->second.bounds.transformed(inst->objectToWorld));
    }

    BvhBuildResult tlas = BvhBuilder::build(bounds, 1);
    if (tlas.nodes.empty())
        tlas.nodes.push_back(sentinelNode()); // empty scene: safe no-op root

    // Instances are stored in TLAS leaf order so leaves index directly.
    std::vector<GpuInstance> gpuInstances;
    gpuInstances.reserve(flat.size());
    for (uint32_t orderedIdx : tlas.primOrder) {
        const InstanceData* inst = flat[orderedIdx];
        GpuInstance gi{};
        gi.objectToWorld = inst->objectToWorld;
        gi.worldToObject = glm::inverse(inst->objectToWorld);
        gi.blasRoot = m_blas[inst->mesh].rootIndexGlobal;
        auto matIt = m_materialIndex.find(inst->material);
        gi.materialIndex = (matIt != m_materialIndex.end()) ? matIt->second : 0;
        gpuInstances.push_back(gi);
    }
    // Remap TLAS leaves: leaf ranges are [begin,count) in primOrder space and
    // count==1, so leaf -> position in gpuInstances == leaf's begin already.
    // Rebase TLAS child indices behind the BLAS region.
    for (BvhNode& n : tlas.nodes) {
        const uint32_t count = n.packed >> 2;
        if (count == 0) n.leftOrPrim += m_tlasRoot;
    }

    m_instanceCount = static_cast<uint32_t>(gpuInstances.size());

    const VkDeviceSize tlasBytes  = tlas.nodes.size() * sizeof(BvhNode);
    const VkDeviceSize tlasOffset = VkDeviceSize(m_tlasRoot) * sizeof(BvhNode);
    if (m_nodesBuf.size() < tlasOffset + tlasBytes) {
        // Instance count grew beyond reserved TLAS capacity: force full repack.
        scene.markDirty(DirtyGeometry);
        packGeometry(scene);
        packInstances(scene);
        return;
    }
    if (!tlas.nodes.empty())
        m_gpu.uploadToBuffer(m_nodesBuf, tlas.nodes.data(), tlasBytes, tlasOffset);

    m_descriptorsDirty |= ensureBuffer(m_gpu, m_instancesBuf,
                                       gpuInstances.size() * sizeof(GpuInstance));
    if (!gpuInstances.empty())
        m_gpu.uploadToBuffer(m_instancesBuf, gpuInstances.data(),
                             gpuInstances.size() * sizeof(GpuInstance));
}

void Renderer::packMaterials(Scene& scene) {
    std::map<MaterialId, const MaterialDesc*> sorted;
    for (auto& [id, mat] : scene.materials()) sorted[id] = &mat;

    auto texSlot = [&](TextureId id) -> int32_t {
        const uint32_t slot = scene.textureSlot(id);
        return (slot == ~0u || slot >= kMaxTextures) ? -1 : static_cast<int32_t>(slot);
    };

    std::vector<GpuMaterial> mats;
    mats.reserve(sorted.size() + 1);
    m_materialIndex.clear();

    // Index 0 is always a neutral default for instances with no material.
    GpuMaterial def{};
    def.baseColorOpacity = vec4(0.8f, 0.8f, 0.8f, 1.0f);
    def.emissionRoughness = vec4(0, 0, 0, 0.5f);
    def.params = vec4(0, 0, 1.45f, 0);
    def.baseColorTex = def.roughnessTex = def.metallicTex = def.normalTex = def.emissionTex = -1;
    mats.push_back(def);

    for (auto& [id, m] : sorted) {
        GpuMaterial g{};
        g.baseColorOpacity = vec4(m->baseColor[0], m->baseColor[1], m->baseColor[2], m->opacity);
        g.emissionRoughness = vec4(m->emission[0], m->emission[1], m->emission[2],
                                   std::clamp(m->roughness, 0.0f, 1.0f));
        g.params = vec4(std::clamp(m->metallic, 0.0f, 1.0f),
                        std::clamp(m->transmission, 0.0f, 1.0f),
                        std::max(m->ior, 1.0f), 0.0f);
        g.baseColorTex = texSlot(m->baseColorTex);
        g.roughnessTex = texSlot(m->roughnessTex);
        g.metallicTex  = texSlot(m->metallicTex);
        g.normalTex    = texSlot(m->normalTex);
        g.emissionTex  = texSlot(m->emissionTex);
        m_materialIndex[id] = static_cast<uint32_t>(mats.size());
        mats.push_back(g);
    }

    m_descriptorsDirty |= ensureBuffer(m_gpu, m_materialsBuf, mats.size() * sizeof(GpuMaterial));
    m_gpu.uploadToBuffer(m_materialsBuf, mats.data(), mats.size() * sizeof(GpuMaterial));
}

void Renderer::packLights(Scene& scene) {
    std::vector<GpuLight> lights;

    // Environment occupies slot 0 when present, so NEE light picking and the
    // BSDF-side MIS pdf agree on pick probability (see lights.glsl).
    if (scene.hasEnvironment()) {
        GpuLight g{};
        g.type = GpuLightEnv;
        lights.push_back(g);
    }

    std::map<LightId, const LightDesc*> sorted;
    for (auto& [id, l] : scene.lights()) sorted[id] = &l;

    for (auto& [id, l] : sorted) {
        GpuLight g{};
        switch (l->type) {
            case LightType::Sun: {
                g.type = GpuLightSun;
                const vec3 d = glm::normalize(vec3(l->direction[0], l->direction[1], l->direction[2]));
                g.p0 = vec4(d, std::cos(std::max(l->angularRadius, 1e-4f)));
                g.p1 = vec4(l->radiance[0], l->radiance[1], l->radiance[2], 0);
                break;
            }
            case LightType::Point: {
                g.type = GpuLightPoint;
                g.p0 = vec4(l->position[0], l->position[1], l->position[2], 0);
                g.p1 = vec4(l->radiance[0], l->radiance[1], l->radiance[2], 0);
                break;
            }
            case LightType::Rect: {
                g.type = GpuLightRect;
                const vec3 e0(l->edge0[0], l->edge0[1], l->edge0[2]);
                const vec3 e1(l->edge1[0], l->edge1[1], l->edge1[2]);
                const float area = glm::length(glm::cross(e0, e1));
                g.p0 = vec4(l->corner[0], l->corner[1], l->corner[2], area);
                g.p1 = vec4(e0, 0);
                g.p2 = vec4(e1, 0);
                g.p3 = vec4(l->radiance[0], l->radiance[1], l->radiance[2],
                            l->twoSided ? 1.0f : 0.0f);
                break;
            }
        }
        lights.push_back(g);
    }

    m_lightCount = static_cast<uint32_t>(lights.size());
    m_descriptorsDirty |= ensureBuffer(m_gpu, m_lightsBuf,
                                       std::max<size_t>(lights.size(), 1) * sizeof(GpuLight));
    if (!lights.empty())
        m_gpu.uploadToBuffer(m_lightsBuf, lights.data(), lights.size() * sizeof(GpuLight));
}

void Renderer::packEnvironment(Scene& scene) {
    m_hasEnv = scene.hasEnvironment();
    if (!m_hasEnv) {
        packLights(scene); // env slot may have been removed
        return;
    }

    const TextureData& env = scene.environment();
    const uint32_t w = env.desc.width, h = env.desc.height;
    const float* px = reinterpret_cast<const float*>(env.data.data());

    m_envImage.create(m_gpu, w, h, VK_FORMAT_R32G32B32A32_SFLOAT,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    m_gpu.uploadToImage(m_envImage, px, size_t(w) * h * 16, false);

    // Luminance CDFs weighted by sin(theta) for equirectangular importance
    // sampling. Layout: [conditional h*w][marginal h]. PDF derivation in
    // lights.glsl relies on envIntegral = ∫ luminance dω.
    std::vector<float> cdf(size_t(w) * h + h);
    float* conditional = cdf.data();
    float* marginal = cdf.data() + size_t(w) * h;

    const float dTheta = glm::pi<float>() / h;
    const float dPhi = 2.0f * glm::pi<float>() / w;
    double integral = 0.0;
    std::vector<double> rowSums(h, 0.0);

    for (uint32_t y = 0; y < h; ++y) {
        const float sinTheta = std::sin((y + 0.5f) * dTheta);
        double rowAccum = 0.0;
        for (uint32_t x = 0; x < w; ++x) {
            const float* p = px + (size_t(y) * w + x) * 4;
            const float lum = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
            rowAccum += double(lum) * sinTheta;
            conditional[size_t(y) * w + x] = static_cast<float>(rowAccum);
        }
        rowSums[y] = rowAccum;
        integral += rowAccum * dTheta * dPhi;
        // Normalize the row CDF.
        const float inv = rowAccum > 0.0 ? float(1.0 / rowAccum) : 0.0f;
        for (uint32_t x = 0; x < w; ++x) conditional[size_t(y) * w + x] *= inv;
    }
    double marginAccum = 0.0, marginTotal = 0.0;
    for (uint32_t y = 0; y < h; ++y) marginTotal += rowSums[y];
    for (uint32_t y = 0; y < h; ++y) {
        marginAccum += rowSums[y];
        marginal[y] = marginTotal > 0.0 ? float(marginAccum / marginTotal) : 0.0f;
    }

    m_envCdfW = w; m_envCdfH = h;
    m_envIntegral = std::max(static_cast<float>(integral), 1e-8f);

    m_descriptorsDirty |= ensureBuffer(m_gpu, m_envCdfBuf, cdf.size() * sizeof(float));
    m_gpu.uploadToBuffer(m_envCdfBuf, cdf.data(), cdf.size() * sizeof(float));
    m_descriptorsDirty = true; // env image view changed

    packLights(scene); // ensure env light entry exists
}

void Renderer::uploadTextures(Scene& scene) {
    const auto& textures = scene.textures();
    const size_t slotCount = std::min(textures.size(), size_t(kMaxTextures));

    if (m_textures.size() < slotCount) {
        m_textures.resize(slotCount);
        m_textureGenerations.resize(slotCount, 0);
    }

    for (size_t slot = 0; slot < slotCount; ++slot) {
        const TextureData& t = textures[slot];
        if (m_textureGenerations[slot] == t.generation) continue; // unchanged since last upload

        if (t.data.empty()) {
            // Removed: revert the slot to the dummy texture (PRD §8 A3).
            m_textures[slot] = Image{};
            m_textureGenerations[slot] = t.generation;
            m_descriptorsDirty = true;
            continue;
        }

        VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
        size_t pixelBytes = 4;
        if (t.desc.format == TextureFormat::RGBA8_SRGB) fmt = VK_FORMAT_R8G8B8A8_SRGB;
        if (t.desc.format == TextureFormat::RGBA32F) { fmt = VK_FORMAT_R32G32B32A32_SFLOAT; pixelBytes = 16; }

        uint32_t mips = 1;
        if (t.desc.generateMips && t.desc.format != TextureFormat::RGBA32F) {
            const uint32_t dim = std::max(t.desc.width, t.desc.height);
            mips = 1 + static_cast<uint32_t>(std::floor(std::log2(double(dim))));
        }

        Image img;
        img.create(m_gpu, t.desc.width, t.desc.height, fmt,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT, mips);
        m_gpu.uploadToImage(img, t.data.data(),
                            size_t(t.desc.width) * t.desc.height * pixelBytes, mips > 1);
        m_textures[slot] = std::move(img);
        m_textureGenerations[slot] = t.generation;
        m_descriptorsDirty = true;
    }
    if (textures.size() > kMaxTextures)
        log(LogLevel::Warn, "Texture count exceeds kMaxTextures; extra textures ignored");
}

// ---------------------------------------------------------------- frames --
void Renderer::fillGlobals(const Scene& scene, GpuGlobals& g) const {
    vec3 pos, forward, upHint;
    float left, right, bottom, top;
    bool orthographic = false;

    if (scene.usesCameraEx()) {
        const CameraDescEx& c = scene.cameraEx();
        pos = vec3(c.position[0], c.position[1], c.position[2]);
        forward = glm::normalize(vec3(c.forward[0], c.forward[1], c.forward[2]));
        upHint = vec3(c.up[0], c.up[1], c.up[2]);
        left = c.left; right = c.right; bottom = c.bottom; top = c.top;
        orthographic = (c.projection == CameraProjection::Orthographic);
    } else {
        // Legacy look-at + vertical FOV: derive a symmetric dist=1 frustum
        // slice from the live render-settings aspect ratio (PRD §8 A2).
        const CameraDesc& c = scene.camera();
        pos = vec3(c.position[0], c.position[1], c.position[2]);
        const vec3 target(c.target[0], c.target[1], c.target[2]);
        forward = glm::normalize(target - pos);
        upHint = vec3(c.up[0], c.up[1], c.up[2]);

        const float aspect = float(m_settings.width) / float(m_settings.height);
        const float tanHalf = std::tan(glm::radians(c.fovYDeg) * 0.5f);
        right = tanHalf * aspect; left = -right;
        top = tanHalf; bottom = -top;
    }

    vec3 rightVec = glm::cross(forward, upHint);
    rightVec = (glm::length(rightVec) < 1e-6f) ? vec3(1, 0, 0) : glm::normalize(rightVec);
    const vec3 upVec = glm::cross(rightVec, forward);

    g.camPos = vec4(pos, 0);
    g.camRight = vec4(rightVec, 0);
    g.camUp = vec4(upVec, 0);
    g.camForward = vec4(forward, 0);
    g.camFrustum = vec4(left, right, bottom, top);
    g.width = m_settings.width;
    g.height = m_settings.height;
    g.frameIndex = m_frameIndex;
    g.maxBounces = std::min(m_settings.maxBounces, kMaxBouncesCap);
    g.lightCount = m_lightCount;
    g.envCdfWidth = m_envCdfW;
    g.envCdfHeight = m_envCdfH;
    g.flags = (m_hasEnv ? FlagHasEnv : 0) | (m_useDenoised ? FlagUseDenoised : 0) |
              (orthographic ? FlagOrthographic : 0);
    g.envRotation = scene.envRotation();
    g.envIntensity = scene.envIntensity();
    g.envIntegral = m_envIntegral;
    g.exposure = std::exp2(m_settings.exposureEV);
    g.fireflyClamp = m_settings.fireflyClamp;
    g.tonemapMode = static_cast<uint32_t>(m_settings.tonemap);
    g.tlasRoot = m_tlasRoot;
    g.debugView = static_cast<uint32_t>(m_settings.debugView);
}

void Renderer::resetAccumulation() {
    m_frameIndex = 0;
    m_useDenoised = false;
    m_nextDenoiseAt = 8;
    ++m_accumEpoch;
}

Result Renderer::renderFrame(Scene& scene, FrameInfo& out) {
    const auto t0 = std::chrono::high_resolution_clock::now();

    syncScene(scene);

    const bool converged = m_frameIndex >= m_settings.sppLimit;
    if (!converged) {
        GpuGlobals g{};
        fillGlobals(scene, g);
        std::memcpy(m_globalsBuf.mapped(), &g, sizeof(g));

        const uint32_t gx = (m_settings.width + 7) / 8;
        const uint32_t gy = (m_settings.height + 7) / 8;

        // Split the pathtrace dispatch (the expensive part) into row-strip
        // tiles, each its own submit+wait, instead of one command buffer
        // covering the whole frame (PRD §10 R6 / §8 A6): a single huge
        // dispatch can starve the GPU long enough to trip Rhino's own UI
        // rendering or the OS's watchdog when sharing the device. Fixed
        // 4-way split for v1. The tile's row offset goes in via a push
        // constant added to gl_GlobalInvocationID.y in the shader — plain
        // vkCmdDispatch every time, base always zero (see the shader comment
        // for why vkCmdDispatchBase was tried and dropped).
        constexpr uint32_t kTileCount = 4;
        const uint32_t rowsPerTile = (gy + kTileCount - 1) / kTileCount;

        for (uint32_t baseRow = 0, tile = 0; baseRow < gy; baseRow += rowsPerTile, ++tile) {
            const uint32_t rows = std::min(rowsPerTile, gy - baseRow);
            m_gpu.immediateSubmit([&](VkCommandBuffer cmd) {
                if (tile == 0) {
                    // Output image content is fully rewritten by resolve: discard.
                    GpuContext::imageBarrier(cmd, m_outputImage.handle(),
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        0, VK_ACCESS_SHADER_WRITE_BIT);
                }
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        m_pipeLayout, 0, 1, &m_descSet, 0, nullptr);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pathtracePipe);
                vkCmdPushConstants(cmd, m_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t), &baseRow);
                vkCmdDispatch(cmd, gx, rows, 1);
            });
        }

        m_gpu.immediateSubmit([&](VkCommandBuffer cmd) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    m_pipeLayout, 0, 1, &m_descSet, 0, nullptr);

            VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &barrier, 0, nullptr, 0, nullptr);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_resolvePipe);
            vkCmdDispatch(cmd, gx, gy, 1);
        });

        ++m_frameIndex;
    }

    if (m_settings.denoise) maybeDenoise();

    const auto t1 = std::chrono::high_resolution_clock::now();
    out.spp = m_frameIndex;
    out.converged = m_frameIndex >= m_settings.sppLimit;
    out.denoised = m_useDenoised;
    out.frameMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
    return Result::Success;
}

void Renderer::maybeDenoise() {
    if (!m_denoiser || !m_denoiser->available() || m_frameIndex == 0) return;

    // Pick up a finished result.
    std::vector<float> denoised;
    if (m_denoiser->tryFetch(m_accumEpoch, denoised)) {
        const size_t pixels = size_t(m_settings.width) * m_settings.height;
        std::vector<vec4> packed(pixels);
        for (size_t i = 0; i < pixels; ++i)
            packed[i] = vec4(denoised[i * 3], denoised[i * 3 + 1], denoised[i * 3 + 2], 1.0f);
        m_gpu.uploadToBuffer(m_denoisedBuf, packed.data(), packed.size() * sizeof(vec4));
        m_useDenoised = true;
        m_nextDenoiseAt = std::min<uint32_t>(m_frameIndex * 2, m_settings.sppLimit);
    }

    // Kick a new job when due (re-denoise at doubling spp: 8, 16, 32, ...).
    if (m_frameIndex >= m_nextDenoiseAt && m_denoiser->idle()) {
        const size_t pixels = size_t(m_settings.width) * m_settings.height;
        std::vector<vec4> accum(pixels), albedo(pixels), normal(pixels);
        m_gpu.readbackBuffer(m_accumBuf, accum.data(), pixels * 16);
        m_gpu.readbackBuffer(m_aovAlbedoBuf, albedo.data(), pixels * 16);
        m_gpu.readbackBuffer(m_aovNormalBuf, normal.data(), pixels * 16);

        std::vector<float> color3(pixels * 3), albedo3(pixels * 3), normal3(pixels * 3);
        const float inv = 1.0f / float(m_frameIndex);
        for (size_t i = 0; i < pixels; ++i) {
            for (int c = 0; c < 3; ++c) {
                color3[i * 3 + c]  = accum[i][c] * inv;
                albedo3[i * 3 + c] = albedo[i][c] * inv;
                normal3[i * 3 + c] = normal[i][c] * inv;
            }
        }
        if (!m_denoiser->start(m_accumEpoch, m_settings.width, m_settings.height,
                               std::move(color3), std::move(albedo3), std::move(normal3)))
            return;
        // Next attempt happens automatically once this job completes.
        m_nextDenoiseAt = std::min<uint32_t>(std::max(m_nextDenoiseAt * 2, 16u),
                                             m_settings.sppLimit);
    }
}

// Readback semantics (PRD §8 A7): row-major, top-down (row 0 = top of the
// image, matching camFrustum's top/bottom mapping in the ray-gen shader —
// see pathtrace.comp), alpha always 1.0 in v1 (no partial coverage/matte).
// Both paths reuse a persistent CPU-side buffer across calls; nothing here
// allocates fresh heap memory per frame, since this runs on every render in
// a Rhino integration's main loop.
Result Renderer::readback(ReadbackFormat fmt, void* dst, size_t dstSize) {
    const size_t pixels = size_t(m_settings.width) * m_settings.height;
    if (fmt == ReadbackFormat::RGBA8_SRGB) {
        if (dstSize < pixels * 4) return Result::ErrorInvalidArgument;
        m_gpu.readbackImage(m_outputImage, m_readbackBuf);
        std::memcpy(dst, m_readbackBuf.mapped(), pixels * 4);
        return Result::Success;
    }
    // RGBA32F_LINEAR: average radiance straight from the accumulation buffer.
    if (dstSize < pixels * 16) return Result::ErrorInvalidArgument;
    if (m_readbackScratch.size() < pixels) m_readbackScratch.resize(pixels);
    m_gpu.readbackBuffer(m_accumBuf, m_readbackScratch.data(), pixels * 16);
    const float inv = m_frameIndex > 0 ? 1.0f / float(m_frameIndex) : 0.0f;
    vec4* out = static_cast<vec4*>(dst);
    for (size_t i = 0; i < pixels; ++i)
        out[i] = vec4(vec3(m_readbackScratch[i]) * inv, 1.0f);
    return Result::Success;
}

} // namespace mrt
