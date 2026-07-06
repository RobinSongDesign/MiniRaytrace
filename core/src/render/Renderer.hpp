#pragma once
// Renderer: owns all GPU scene buffers, the two compute pipelines
// (pathtrace, resolve) and the accumulation state. Synchronisation model is
// deliberately simple for v1: one frame in flight, blocking submits.

#include "mrt/Engine.hpp" // ReadbackFormat
#include "mrt/Scene.hpp"
#include "mrt/SceneTypes.hpp"
#include "../accel/Bvh.hpp" // BvhNode (BlasEntry below)
#include "../gpu/GpuContext.hpp"
#include "GpuTypes.hpp"

#include <memory>
#include <vector>

namespace mrt {

class Denoiser;

class Renderer {
public:
    Renderer(GpuContext& gpu, Denoiser* denoiser);
    ~Renderer();

    Result init(const RenderSettings& settings);
    Result setSettings(const RenderSettings& settings);

    // Consume scene dirty bits: BLAS/TLAS rebuilds, buffer repacks, uploads.
    Result syncScene(Scene& scene);

    Result renderFrame(Scene& scene, FrameInfo& out);
    void   resetAccumulation();

    Result readback(ReadbackFormat fmt, void* dst, size_t dstSize);

    Image&   output()      { return m_outputImage; }
    uint32_t frameIndex() const { return m_frameIndex; }

private:
    void createPipelines();
    void createFrameResources();
    void updateDescriptors();
    void packGeometry(Scene& scene);   // BLAS rebuilds + global buffer repack
    void packInstances(Scene& scene);  // TLAS rebuild + instance buffer
    void packMaterials(Scene& scene);
    void packLights(Scene& scene);
    void packEnvironment(Scene& scene);
    void uploadTextures(Scene& scene);
    void fillGlobals(const Scene& scene, GpuGlobals& g) const;
    void maybeDenoise();

    VkPipeline createComputePipeline(const uint32_t* spv, size_t words, const char* name);

    GpuContext& m_gpu;
    Denoiser*   m_denoiser = nullptr;
    RenderSettings m_settings;

    // Pipelines / descriptors
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipeLayout = VK_NULL_HANDLE;
    VkPipeline            m_pathtracePipe = VK_NULL_HANDLE;
    VkPipeline            m_resolvePipe = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool = VK_NULL_HANDLE;
    VkDescriptorSet       m_descSet = VK_NULL_HANDLE;

    // Frame resources (sized by settings.width/height)
    Buffer m_globalsBuf, m_accumBuf, m_aovAlbedoBuf, m_aovNormalBuf, m_denoisedBuf;
    Image  m_outputImage;
    Buffer m_readbackBuf;

    // Scene buffers
    Buffer m_nodesBuf, m_trianglesBuf, m_positionsBuf, m_normalsBuf;
    Buffer m_uvsBuf, m_tangentsBuf, m_instancesBuf, m_materialsBuf;
    Buffer m_lightsBuf, m_envCdfBuf;
    Image  m_envImage;
    Image  m_dummyTexture;
    std::vector<Image> m_textures;        // index == scene texture slot

    // CPU-side packing state
    struct BlasEntry {
        std::vector<BvhNode>  nodes;     // local indices
        std::vector<uint32_t> triOrder;  // local triangle reorder
        Aabb     bounds;
        uint32_t rootIndexGlobal = 0;    // assigned at pack time
    };
    std::unordered_map<MeshId, BlasEntry> m_blas;
    std::unordered_map<MeshId, uint32_t>  m_meshVertexBase; // global vertex offsets
    std::unordered_map<MaterialId, uint32_t> m_materialIndex;
    uint32_t m_tlasRoot = 0;
    uint32_t m_instanceCount = 0;
    uint32_t m_lightCount = 0;
    uint32_t m_envCdfW = 0, m_envCdfH = 0;
    float    m_envIntegral = 1.0f;
    bool     m_hasEnv = false;
    bool     m_descriptorsDirty = true;

    // Accumulation / denoise state
    uint32_t m_frameIndex = 0;       // == accumulated spp (1 spp per frame)
    uint32_t m_nextDenoiseAt = 8;
    bool     m_useDenoised = false;
    uint64_t m_accumEpoch = 0;       // bumped on every reset; cancels stale denoises
};

} // namespace mrt
