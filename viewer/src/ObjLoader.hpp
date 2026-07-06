#pragma once
// OBJ/MTL -> scene. Splits by material, maps MTL Phong fields to the
// PBR BSDF (PRD §6.2), loads referenced textures.

#include <mrt/Engine.hpp>

#include <string>
#include <utility>
#include <vector>

namespace viewer {

struct LoadResult {
    bool ok = false;
    mrt::vec3 boundsLo{ 0.0f };
    mrt::vec3 boundsHi{ 0.0f };
    size_t triangleCount = 0;
    // Handles created in the scene, for later editing/removal by the UI.
    std::vector<mrt::MeshId>     meshes;
    std::vector<mrt::InstanceId> instances;
    std::vector<std::pair<mrt::MaterialId, std::string>> materials; // id + name
};

LoadResult loadObjIntoScene(mrt::Scene& scene, const std::string& objPath);

// Load an equirectangular .hdr/.exr(.hdr via stb) into the scene environment.
bool loadEnvironment(mrt::Scene& scene, const std::string& hdrPath,
                     float rotationRad = 0.0f, float intensity = 1.0f);

// Load a standalone LDR image as a scene texture (UI: assign to a material).
mrt::TextureId loadImageTexture(mrt::Scene& scene, const std::string& path, bool srgb);

} // namespace viewer
