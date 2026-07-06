#include "ObjLoader.hpp"

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <unordered_map>

namespace viewer {

namespace {

namespace fs = std::filesystem;

// Texture cache: path -> TextureId (avoids duplicate uploads).
mrt::TextureId loadTexture(mrt::Scene& scene, const fs::path& path, bool srgb,
                           std::unordered_map<std::string, mrt::TextureId>& cache) {
    const std::string key = path.string() + (srgb ? "#srgb" : "#unorm");
    if (auto it = cache.find(key); it != cache.end()) return it->second;

    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load(path.string().c_str(), &w, &h, &comp, 4);
    if (!pixels) {
        fprintf(stderr, "[viewer] texture load failed: %s\n", path.string().c_str());
        cache[key] = mrt::kInvalidId;
        return mrt::kInvalidId;
    }

    mrt::TextureDesc d;
    d.width = uint32_t(w);
    d.height = uint32_t(h);
    d.format = srgb ? mrt::TextureFormat::RGBA8_SRGB : mrt::TextureFormat::RGBA8_UNORM;
    d.pixels = pixels;
    d.generateMips = true;
    const mrt::TextureId id = scene.addTexture(d);
    stbi_image_free(pixels);
    cache[key] = id;
    return id;
}

// MTL (Phong) -> PBR mapping (PRD §6.2). PBR extension fields win if present.
mrt::MaterialDesc convertMaterial(const tinyobj::material_t& m,
                                  const fs::path& baseDir,
                                  mrt::Scene& scene,
                                  std::unordered_map<std::string, mrt::TextureId>& cache) {
    mrt::MaterialDesc d;
    d.baseColor[0] = m.diffuse[0];
    d.baseColor[1] = m.diffuse[1];
    d.baseColor[2] = m.diffuse[2];

    if (m.roughness > 0.0f) {
        d.roughness = m.roughness;                       // PBR extension `Pr`
    } else {
        const float ns = std::max(m.shininess, 0.0f);    // Ns -> roughness
        d.roughness = std::clamp(std::sqrt(2.0f / (ns + 2.0f)), 0.03f, 1.0f);
    }

    if (m.metallic > 0.0f) {
        d.metallic = m.metallic;                         // PBR extension `Pm`
    } else {
        // Heuristic: strong specular + dark diffuse suggests metal.
        const float specLum = 0.2126f * m.specular[0] + 0.7152f * m.specular[1] + 0.0722f * m.specular[2];
        const float diffLum = 0.2126f * m.diffuse[0] + 0.7152f * m.diffuse[1] + 0.0722f * m.diffuse[2];
        if (specLum > 0.5f && diffLum < 0.1f) d.metallic = 1.0f;
    }

    d.opacity = std::clamp(m.dissolve, 0.0f, 1.0f);
    d.ior = std::max(m.ior, 1.0f);
    // Tf < 1 on any channel signals transmission in common exporter conventions.
    const float tf = (m.transmittance[0] + m.transmittance[1] + m.transmittance[2]) / 3.0f;
    if (tf > 0.01f && tf < 1.0f) d.transmission = tf;

    d.emission[0] = m.emission[0];
    d.emission[1] = m.emission[1];
    d.emission[2] = m.emission[2];

    auto resolve = [&](const std::string& name) { return baseDir / name; };
    if (!m.diffuse_texname.empty()) {
        d.baseColorTex = loadTexture(scene, resolve(m.diffuse_texname), true, cache);
        // Some exporters write Kd 0 0 0 alongside map_Kd; the texture would be
        // multiplied to black. Treat the texture as authoritative in that case.
        if (d.baseColor[0] + d.baseColor[1] + d.baseColor[2] < 0.01f)
            d.baseColor[0] = d.baseColor[1] = d.baseColor[2] = 1.0f;
    }
    if (!m.roughness_texname.empty())
        d.roughnessTex = loadTexture(scene, resolve(m.roughness_texname), false, cache);
    if (!m.metallic_texname.empty())
        d.metallicTex = loadTexture(scene, resolve(m.metallic_texname), false, cache);
    if (!m.normal_texname.empty())
        d.normalTex = loadTexture(scene, resolve(m.normal_texname), false, cache);
    else if (!m.bump_texname.empty())
        d.normalTex = loadTexture(scene, resolve(m.bump_texname), false, cache);
    if (!m.emissive_texname.empty())
        d.emissionTex = loadTexture(scene, resolve(m.emissive_texname), true, cache);
    return d;
}

// Fallback for OBJ files that ship without an MTL: probe the
// tinyrenderer-style naming convention next to the .obj
// (<name>_diffuse / _nm_tangent / _spec / _glow, any common image format).
mrt::MaterialDesc conventionMaterial(const fs::path& objPath, mrt::Scene& scene,
                                     std::unordered_map<std::string, mrt::TextureId>& cache) {
    mrt::MaterialDesc d;
    const fs::path dir = objPath.parent_path();
    const std::string stem = objPath.stem().string();

    auto find = [&](const char* suffix) -> fs::path {
        for (const char* ext : { ".tga", ".png", ".jpg", ".jpeg" }) {
            const fs::path p = dir / (stem + suffix + ext);
            if (fs::exists(p)) return p;
        }
        return {};
    };

    if (const fs::path p = find("_diffuse"); !p.empty()) {
        d.baseColorTex = loadTexture(scene, p, true, cache);
        d.baseColor[0] = d.baseColor[1] = d.baseColor[2] = 1.0f; // texture is the color
    }
    if (const fs::path p = find("_nm_tangent"); !p.empty())
        d.normalTex = loadTexture(scene, p, false, cache);
    if (const fs::path p = find("_glow"); !p.empty()) {
        d.emissionTex = loadTexture(scene, p, true, cache);
        if (d.emissionTex != mrt::kInvalidId)
            d.emission[0] = d.emission[1] = d.emission[2] = 2.0f; // scales the glow texture
    }
    // _spec maps don't translate directly to roughness; use a sane constant.
    d.roughness = 0.55f;
    if (d.baseColorTex != mrt::kInvalidId)
        fprintf(stderr, "[viewer] no MTL: using convention textures for %s\n", stem.c_str());
    return d;
}

// Vertex dedup key: position/normal/uv index triple.
struct IndexKey {
    int v, n, t;
    bool operator==(const IndexKey& o) const { return v == o.v && n == o.n && t == o.t; }
};
struct IndexKeyHash {
    size_t operator()(const IndexKey& k) const {
        return std::hash<int64_t>()((int64_t(k.v) * 73856093LL) ^
                                    (int64_t(k.n) * 19349663LL) ^
                                    (int64_t(k.t) * 83492791LL));
    }
};

} // namespace

LoadResult loadObjIntoScene(mrt::Scene& scene, const std::string& objPath) {
    LoadResult result;

    tinyobj::ObjReaderConfig cfg;
    cfg.triangulate = true;
    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(objPath, cfg)) {
        fprintf(stderr, "[viewer] OBJ load failed: %s\n", reader.Error().c_str());
        return result;
    }
    if (!reader.Warning().empty())
        fprintf(stderr, "[viewer] OBJ warning: %s\n", reader.Warning().c_str());

    const auto& attrib = reader.GetAttrib();
    const auto& shapes = reader.GetShapes();
    const auto& objMaterials = reader.GetMaterials();
    const fs::path baseDir = fs::path(objPath).parent_path();

    // Materials (index -1 = default).
    std::unordered_map<std::string, mrt::TextureId> texCache;
    std::vector<mrt::MaterialId> materialIds;
    materialIds.reserve(objMaterials.size());
    for (const auto& m : objMaterials) {
        const mrt::MaterialId id = scene.addMaterial(convertMaterial(m, baseDir, scene, texCache));
        materialIds.push_back(id);
        result.materials.emplace_back(id, m.name.empty() ? "material" : m.name);
    }
    // No MTL at all -> probe convention-named textures next to the obj.
    const mrt::MaterialId defaultMat = scene.addMaterial(
        objMaterials.empty() ? conventionMaterial(fs::path(objPath), scene, texCache)
                             : mrt::MaterialDesc{});
    result.materials.emplace_back(defaultMat, "default");

    // One mesh per (shape, material) bucket: GPU side is single-material meshes.
    struct Bucket {
        std::vector<float> positions, normals, uvs;
        std::vector<uint32_t> indices;
        std::unordered_map<IndexKey, uint32_t, IndexKeyHash> dedup;
        bool hasNormals = true, hasUvs = true;
    };

    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(-std::numeric_limits<float>::max());

    for (const auto& shape : shapes) {
        std::unordered_map<int, Bucket> buckets; // matId -> bucket
        const size_t faceCount = shape.mesh.num_face_vertices.size();
        for (size_t f = 0; f < faceCount; ++f) {
            const int matId = f < shape.mesh.material_ids.size() ? shape.mesh.material_ids[f] : -1;
            Bucket& b = buckets[matId];
            for (int corner = 0; corner < 3; ++corner) {
                const tinyobj::index_t idx = shape.mesh.indices[f * 3 + corner];
                IndexKey key{ idx.vertex_index, idx.normal_index, idx.texcoord_index };
                auto it = b.dedup.find(key);
                if (it != b.dedup.end()) {
                    b.indices.push_back(it->second);
                    continue;
                }
                const uint32_t newIndex = uint32_t(b.positions.size() / 3);
                b.dedup[key] = newIndex;
                b.indices.push_back(newIndex);

                const float* p = &attrib.vertices[3 * size_t(idx.vertex_index)];
                b.positions.insert(b.positions.end(), { p[0], p[1], p[2] });
                lo = glm::min(lo, glm::vec3(p[0], p[1], p[2]));
                hi = glm::max(hi, glm::vec3(p[0], p[1], p[2]));

                if (idx.normal_index >= 0) {
                    const float* n = &attrib.normals[3 * size_t(idx.normal_index)];
                    b.normals.insert(b.normals.end(), { n[0], n[1], n[2] });
                } else {
                    b.hasNormals = false;
                    b.normals.insert(b.normals.end(), { 0, 0, 0 });
                }
                if (idx.texcoord_index >= 0) {
                    const float* t = &attrib.texcoords[2 * size_t(idx.texcoord_index)];
                    b.uvs.insert(b.uvs.end(), { t[0], 1.0f - t[1] }); // OBJ v is bottom-up
                } else {
                    b.hasUvs = false;
                    b.uvs.insert(b.uvs.end(), { 0, 0 });
                }
            }
        }

        for (auto& [matId, b] : buckets) {
            if (b.indices.empty()) continue;
            mrt::MeshDesc md;
            md.vertexCount = uint32_t(b.positions.size() / 3);
            md.indexCount = uint32_t(b.indices.size());
            md.positions = b.positions.data();
            md.normals = b.hasNormals ? b.normals.data() : nullptr;
            md.uvs = b.hasUvs ? b.uvs.data() : nullptr;
            md.indices = b.indices.data();

            const mrt::MeshId mesh = scene.addMesh(md);
            const mrt::MaterialId mat = (matId >= 0 && size_t(matId) < materialIds.size())
                                            ? materialIds[size_t(matId)] : defaultMat;
            const float identity[12] = { 1,0,0,0, 0,1,0,0, 0,0,1,0 };
            result.meshes.push_back(mesh);
            result.instances.push_back(scene.addInstance(mesh, identity, mat));
            result.triangleCount += b.indices.size() / 3;
        }
    }

    result.ok = result.triangleCount > 0;
    result.boundsLo = lo;
    result.boundsHi = hi;
    return result;
}

mrt::TextureId loadImageTexture(mrt::Scene& scene, const std::string& path, bool srgb) {
    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!pixels) {
        fprintf(stderr, "[viewer] texture load failed: %s\n", path.c_str());
        return mrt::kInvalidId;
    }
    mrt::TextureDesc d;
    d.width = uint32_t(w);
    d.height = uint32_t(h);
    d.format = srgb ? mrt::TextureFormat::RGBA8_SRGB : mrt::TextureFormat::RGBA8_UNORM;
    d.pixels = pixels;
    d.generateMips = true;
    const mrt::TextureId id = scene.addTexture(d);
    stbi_image_free(pixels);
    return id;
}

bool loadEnvironment(mrt::Scene& scene, const std::string& hdrPath,
                     float rotationRad, float intensity) {
    int w = 0, h = 0, comp = 0;
    float* pixels = stbi_loadf(hdrPath.c_str(), &w, &h, &comp, 4);
    if (!pixels) {
        fprintf(stderr, "[viewer] HDR load failed: %s\n", hdrPath.c_str());
        return false;
    }
    mrt::TextureDesc d;
    d.width = uint32_t(w);
    d.height = uint32_t(h);
    d.format = mrt::TextureFormat::RGBA32F;
    d.pixels = pixels;
    d.generateMips = false;
    scene.setEnvironment(d, rotationRad, intensity);
    stbi_image_free(pixels);
    return true;
}

} // namespace viewer
