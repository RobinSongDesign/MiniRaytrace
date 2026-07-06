#pragma once
// Incremental scene container. Mirrors the Rhino ChangeQueue push model:
// consumers add/update/remove objects, then Engine::renderFrame() syncs dirty
// state to the GPU (BVH rebuilds, buffer uploads) before tracing.

#include "Common.hpp"
#include "MathTypes.hpp"
#include "SceneTypes.hpp"

#include <unordered_map>
#include <vector>

namespace mrt {

// CPU-side storage. Positions/normals are padded to vec4 for std430 friendliness.
struct MeshData {
    std::vector<vec4>     positions;  // w unused
    std::vector<vec4>     normals;    // w unused
    std::vector<vec2>     uvs;        // empty if none
    std::vector<vec4>     tangents;   // w = handedness; empty if no uvs
    std::vector<uint32_t> indices;
    Aabb                  bounds;
    bool                  blasDirty = true;
};

struct InstanceData {
    MeshId     mesh     = kInvalidId;
    MaterialId material = kInvalidId;
    mat4       objectToWorld{ 1.0f };
};

struct TextureData {
    TextureDesc          desc;   // pixels pointer invalid after add; data below owns
    std::vector<uint8_t> data;
    uint32_t             slot = ~0u; // GPU texture array slot, assigned on add
};

enum DirtyBits : uint32_t {
    DirtyNone      = 0,
    DirtyGeometry  = 1u << 0, // mesh added/removed/changed -> BLAS + repack
    DirtyInstances = 1u << 1, // transforms / instance set  -> TLAS
    DirtyMaterials = 1u << 2,
    DirtyLights    = 1u << 3,
    DirtyEnv       = 1u << 4,
    DirtyCamera    = 1u << 5,
    DirtyTextures  = 1u << 6,
    DirtyEnvParams = 1u << 7, // rotation/intensity only: no GPU repack needed
};

class Scene {
public:
    // -- meshes ---------------------------------------------------------
    MeshId addMesh(const MeshDesc& d);
    void   updateMesh(MeshId id, const MeshDesc& d);
    void   removeMesh(MeshId id);

    // -- instances ------------------------------------------------------
    InstanceId addInstance(MeshId mesh, const float xform3x4[12], MaterialId mat);
    void       setInstanceTransform(InstanceId id, const float xform3x4[12]);
    void       removeInstance(InstanceId id);

    // -- materials / textures ------------------------------------------
    MaterialId addMaterial(const MaterialDesc& d);
    void       updateMaterial(MaterialId id, const MaterialDesc& d);
    void       removeMaterial(MaterialId id); // referencing instances fall back to default
    TextureId  addTexture(const TextureDesc& d);

    // -- lights / environment / camera ---------------------------------
    LightId addLight(const LightDesc& d);
    void    updateLight(LightId id, const LightDesc& d);
    void    removeLight(LightId id);
    void    setEnvironment(const TextureDesc& hdri, float rotationRad, float intensity);
    void    setEnvironmentParams(float rotationRad, float intensity); // cheap, no re-upload
    void    clearEnvironment();
    void    setCamera(const CameraDesc& d);

    // -- dirty tracking (consumed by the renderer) ----------------------
    uint32_t dirty() const { return m_dirty; }
    void     clearDirty()  { m_dirty = DirtyNone; }
    void     markDirty(uint32_t bits) { m_dirty |= bits; }

    // -- accessors ------------------------------------------------------
    const std::unordered_map<MeshId, MeshData>&         meshes()    const { return m_meshes; }
    std::unordered_map<MeshId, MeshData>&               meshes()          { return m_meshes; }
    const std::unordered_map<InstanceId, InstanceData>& instances() const { return m_instances; }
    const std::unordered_map<MaterialId, MaterialDesc>& materials() const { return m_materials; }
    const std::unordered_map<LightId, LightDesc>&       lights()    const { return m_lights; }
    const std::vector<TextureData>&                     textures()  const { return m_textures; }
    uint32_t   textureSlot(TextureId id) const; // ~0u if invalid
    const CameraDesc& camera() const { return m_camera; }

    bool        hasEnvironment() const { return m_envValid; }
    const TextureData& environment() const { return m_env; }
    float       envRotation()  const { return m_envRotation; }
    float       envIntensity() const { return m_envIntensity; }

private:
    static void copyMeshData(const MeshDesc& d, MeshData& out);

    uint32_t m_nextId = 1;
    uint32_t m_dirty  = DirtyNone;

    std::unordered_map<MeshId, MeshData>         m_meshes;
    std::unordered_map<InstanceId, InstanceData> m_instances;
    std::unordered_map<MaterialId, MaterialDesc> m_materials;
    std::unordered_map<LightId, LightDesc>       m_lights;
    std::vector<TextureData>                     m_textures;     // slot == index
    std::unordered_map<TextureId, uint32_t>      m_textureSlots; // id -> slot

    CameraDesc  m_camera{};
    TextureData m_env{};
    bool        m_envValid    = false;
    float       m_envRotation = 0.0f;
    float       m_envIntensity = 1.0f;
};

} // namespace mrt
