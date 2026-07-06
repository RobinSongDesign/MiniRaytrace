// Incremental scene container: deep copies, derived attributes (normals,
// tangents), dirty-bit bookkeeping.

#include "mrt/Scene.hpp"

#include <cstring>

namespace mrt {

namespace {

// Area-weighted vertex normals for meshes that arrive without them.
void computeNormals(MeshData& m) {
    m.normals.assign(m.positions.size(), vec4(0));
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        const vec3 p0(m.positions[m.indices[t]]);
        const vec3 p1(m.positions[m.indices[t + 1]]);
        const vec3 p2(m.positions[m.indices[t + 2]]);
        const vec3 n = glm::cross(p1 - p0, p2 - p0); // length == 2*area
        for (int k = 0; k < 3; ++k)
            m.normals[m.indices[t + k]] += vec4(n, 0);
    }
    for (auto& n : m.normals) {
        const vec3 v(n);
        n = vec4(glm::length(v) > 1e-12f ? glm::normalize(v) : vec3(0, 1, 0), 0);
    }
}

// Per-vertex tangents (simplified mikktspace: accumulate per-face, then
// Gram-Schmidt against the normal). Needed by normal mapping.
void computeTangents(MeshData& m) {
    if (m.uvs.empty()) return;
    std::vector<vec3> tan(m.positions.size(), vec3(0));
    std::vector<vec3> bitan(m.positions.size(), vec3(0));

    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        const uint32_t i0 = m.indices[t], i1 = m.indices[t + 1], i2 = m.indices[t + 2];
        const vec3 e1 = vec3(m.positions[i1]) - vec3(m.positions[i0]);
        const vec3 e2 = vec3(m.positions[i2]) - vec3(m.positions[i0]);
        const vec2 d1 = m.uvs[i1] - m.uvs[i0];
        const vec2 d2 = m.uvs[i2] - m.uvs[i0];
        const float det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-12f) continue;
        const float r = 1.0f / det;
        const vec3 T = (e1 * d2.y - e2 * d1.y) * r;
        const vec3 B = (e2 * d1.x - e1 * d2.x) * r;
        for (uint32_t i : { i0, i1, i2 }) { tan[i] += T; bitan[i] += B; }
    }

    m.tangents.resize(m.positions.size());
    for (size_t i = 0; i < m.positions.size(); ++i) {
        const vec3 n(m.normals[i]);
        vec3 t = tan[i] - n * glm::dot(n, tan[i]);
        if (glm::length(t) < 1e-9f)
            t = std::abs(n.x) < 0.9f ? glm::cross(n, vec3(1, 0, 0)) : glm::cross(n, vec3(0, 1, 0));
        t = glm::normalize(t);
        const float handed = glm::dot(glm::cross(n, t), bitan[i]) < 0.0f ? -1.0f : 1.0f;
        m.tangents[i] = vec4(t, handed);
    }
}

} // namespace

void Scene::copyMeshData(const MeshDesc& d, MeshData& out) {
    out.positions.resize(d.vertexCount);
    out.bounds = Aabb{};
    for (uint32_t i = 0; i < d.vertexCount; ++i) {
        const vec3 p(d.positions[i * 3], d.positions[i * 3 + 1], d.positions[i * 3 + 2]);
        out.positions[i] = vec4(p, 0);
        out.bounds.expand(p);
    }
    out.indices.assign(d.indices, d.indices + d.indexCount);

    if (d.normals) {
        out.normals.resize(d.vertexCount);
        for (uint32_t i = 0; i < d.vertexCount; ++i)
            out.normals[i] = vec4(d.normals[i * 3], d.normals[i * 3 + 1], d.normals[i * 3 + 2], 0);
    } else {
        computeNormals(out);
    }

    if (d.uvs) {
        out.uvs.resize(d.vertexCount);
        for (uint32_t i = 0; i < d.vertexCount; ++i)
            out.uvs[i] = vec2(d.uvs[i * 2], d.uvs[i * 2 + 1]);
        computeTangents(out);
    } else {
        out.uvs.clear();
        out.tangents.clear();
    }
    out.blasDirty = true;
}

MeshId Scene::addMesh(const MeshDesc& d) {
    if (!d.positions || !d.indices || d.indexCount % 3 != 0) return kInvalidId;
    const MeshId id = m_nextId++;
    copyMeshData(d, m_meshes[id]);
    markDirty(DirtyGeometry);
    return id;
}

void Scene::updateMesh(MeshId id, const MeshDesc& d) {
    auto it = m_meshes.find(id);
    if (it == m_meshes.end()) return;
    copyMeshData(d, it->second);
    markDirty(DirtyGeometry);
}

void Scene::removeMesh(MeshId id) {
    if (m_meshes.erase(id) == 0) return;
    // Cascade: drop instances referencing the mesh.
    for (auto it = m_instances.begin(); it != m_instances.end();)
        it = (it->second.mesh == id) ? m_instances.erase(it) : std::next(it);
    markDirty(DirtyGeometry | DirtyInstances);
}

InstanceId Scene::addInstance(MeshId mesh, const float xform3x4[12], MaterialId mat) {
    if (!m_meshes.count(mesh)) return kInvalidId;
    const InstanceId id = m_nextId++;
    InstanceData& inst = m_instances[id];
    inst.mesh = mesh;
    inst.material = mat;
    inst.objectToWorld = xform3x4 ? mat4FromRows3x4(xform3x4) : mat4(1.0f);
    markDirty(DirtyInstances);
    return id;
}

void Scene::setInstanceTransform(InstanceId id, const float xform3x4[12]) {
    auto it = m_instances.find(id);
    if (it == m_instances.end() || !xform3x4) return;
    it->second.objectToWorld = mat4FromRows3x4(xform3x4);
    markDirty(DirtyInstances);
}

void Scene::removeInstance(InstanceId id) {
    if (m_instances.erase(id))
        markDirty(DirtyInstances);
}

MaterialId Scene::addMaterial(const MaterialDesc& d) {
    const MaterialId id = m_nextId++;
    m_materials[id] = d;
    markDirty(DirtyMaterials);
    return id;
}

void Scene::updateMaterial(MaterialId id, const MaterialDesc& d) {
    auto it = m_materials.find(id);
    if (it == m_materials.end()) return;
    it->second = d;
    markDirty(DirtyMaterials);
}

void Scene::removeMaterial(MaterialId id) {
    if (m_materials.erase(id))
        markDirty(DirtyMaterials | DirtyInstances);
}

TextureId Scene::addTexture(const TextureDesc& d) {
    if (!d.pixels || d.width == 0 || d.height == 0) return kInvalidId;
    const TextureId id = m_nextId++;
    const size_t pixelBytes = (d.format == TextureFormat::RGBA32F) ? 16 : 4;

    TextureData t;
    t.desc = d;
    t.desc.pixels = nullptr; // ownership transferred to t.data
    t.data.resize(size_t(d.width) * d.height * pixelBytes);
    std::memcpy(t.data.data(), d.pixels, t.data.size());
    t.slot = static_cast<uint32_t>(m_textures.size());

    m_textureSlots[id] = t.slot;
    m_textures.push_back(std::move(t));
    markDirty(DirtyTextures);
    return id;
}

uint32_t Scene::textureSlot(TextureId id) const {
    auto it = m_textureSlots.find(id);
    return it != m_textureSlots.end() ? it->second : ~0u;
}

LightId Scene::addLight(const LightDesc& d) {
    const LightId id = m_nextId++;
    m_lights[id] = d;
    markDirty(DirtyLights);
    return id;
}

void Scene::updateLight(LightId id, const LightDesc& d) {
    auto it = m_lights.find(id);
    if (it == m_lights.end()) return;
    it->second = d;
    markDirty(DirtyLights);
}

void Scene::removeLight(LightId id) {
    if (m_lights.erase(id))
        markDirty(DirtyLights);
}

void Scene::setEnvironment(const TextureDesc& hdri, float rotationRad, float intensity) {
    if (!hdri.pixels || hdri.format != TextureFormat::RGBA32F) return;
    m_env.desc = hdri;
    m_env.desc.pixels = nullptr;
    m_env.data.resize(size_t(hdri.width) * hdri.height * 16);
    std::memcpy(m_env.data.data(), hdri.pixels, m_env.data.size());
    m_envValid = true;
    m_envRotation = rotationRad;
    m_envIntensity = intensity;
    markDirty(DirtyEnv | DirtyLights);
}

void Scene::setEnvironmentParams(float rotationRad, float intensity) {
    if (!m_envValid) return;
    m_envRotation = rotationRad;
    m_envIntensity = intensity;
    markDirty(DirtyEnvParams); // globals-only change
}

void Scene::clearEnvironment() {
    m_envValid = false;
    m_env.data.clear();
    markDirty(DirtyEnv | DirtyLights);
}

void Scene::setCamera(const CameraDesc& d) {
    m_camera = d;
    markDirty(DirtyCamera);
}

} // namespace mrt
