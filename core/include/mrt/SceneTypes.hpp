#pragma once
// Plain descriptor structs used by the incremental scene API.
// All pointers are borrowed for the duration of the call; the scene deep-copies.

#include "Common.hpp"
#include <cstdint>

namespace mrt {

struct MeshDesc {
    uint32_t        vertexCount = 0;
    uint32_t        indexCount  = 0;       // multiple of 3
    const float*    positions   = nullptr; // xyz, required
    const float*    normals     = nullptr; // xyz, optional (computed if null)
    const float*    uvs         = nullptr; // uv,  optional
    const uint32_t* indices     = nullptr; // required
};

enum class TextureFormat : uint32_t {
    RGBA8_SRGB,   // color data (baseColor, emission)
    RGBA8_UNORM,  // data maps (roughness, metallic, normal)
    RGBA32F,      // HDR (environment)
};

struct TextureDesc {
    uint32_t      width = 0, height = 0;
    TextureFormat format = TextureFormat::RGBA8_SRGB;
    const void*   pixels = nullptr; // tightly packed RGBA
    bool          generateMips = true;
};

struct MaterialDesc {
    float baseColor[3]  = { 0.8f, 0.8f, 0.8f };
    float opacity       = 1.0f;
    float roughness     = 0.5f;
    float metallic      = 0.0f;
    float transmission  = 0.0f;
    float ior           = 1.45f;
    float emission[3]   = { 0.0f, 0.0f, 0.0f };
    // kInvalidId = constant value only.
    TextureId baseColorTex = kInvalidId;
    TextureId roughnessTex = kInvalidId;
    TextureId metallicTex  = kInvalidId;
    TextureId normalTex    = kInvalidId;
    TextureId emissionTex  = kInvalidId;
};

enum class LightType : uint32_t { Sun = 0, Point = 1, Rect = 2 };

struct LightDesc {
    LightType type = LightType::Sun;
    // Sun: direction = towards the light, radiance, angularRadius (radians).
    // Point: position, intensity (W/sr).
    // Rect: corner + edge0 + edge1 define the quad, radiance, twoSided.
    float direction[3]     = { 0.0f, 1.0f, 0.0f };
    float position[3]      = { 0.0f, 0.0f, 0.0f };
    float radiance[3]      = { 1.0f, 1.0f, 1.0f };
    float angularRadius    = 0.00465f; // physical sun
    float corner[3]        = { 0, 0, 0 };
    float edge0[3]         = { 1, 0, 0 };
    float edge1[3]         = { 0, 0, 1 };
    bool  twoSided         = false;
};

struct CameraDesc {
    float position[3] = { 0, 0, 5 };
    float target[3]   = { 0, 0, 0 };
    float up[3]       = { 0, 1, 0 };
    float fovYDeg     = 45.0f;
};

enum class CameraProjection : uint32_t { Perspective = 0, Orthographic = 1 };

// Extended camera model (PRD §8 A2): explicit asymmetric frustum + parallel
// projection, matching Rhino's ViewportInfo::GetFrustum(). left/right/bottom/top
// are a slice of the view frustum at distance 1 along `forward` for
// perspective, or absolute half-extents for orthographic — either way they
// map directly to Rhino's frustum values (perspective ones pre-divided by
// `near`). Two-point perspective is expressed naturally via an asymmetric
// frustum with a level `forward`; no separate flag needed.
struct CameraDescEx {
    CameraProjection projection = CameraProjection::Perspective;
    float position[3] = { 0, 0, 5 };
    float forward[3]  = { 0, 0, -1 }; // unit
    float up[3]       = { 0, 1, 0 };  // up hint, orthonormalized against forward
    float left = -1.0f, right = 1.0f, bottom = -1.0f, top = 1.0f;
};

enum class TonemapMode : uint32_t { Linear = 0, Aces = 1 };

// Debug visualisation of the first-hit AOVs (resolve pass).
enum class DebugView : uint32_t { Off = 0, Albedo = 1, Normal = 2 };

struct RenderSettings {
    uint32_t    width        = 1280;
    uint32_t    height       = 720;
    uint32_t    maxBounces   = 8;
    uint32_t    sppLimit     = 1024;     // accumulation stops here
    float       exposureEV   = 0.0f;
    float       fireflyClamp = 50.0f;    // per-sample radiance clamp, 0 = off
    TonemapMode tonemap      = TonemapMode::Aces;
    DebugView   debugView    = DebugView::Off;
    bool        denoise      = true;     // no-op when built without OIDN
};

struct FrameInfo {
    uint32_t spp        = 0;     // accumulated samples per pixel
    bool     converged  = false; // sppLimit reached
    bool     denoised   = false; // resolve currently shows denoised image
    float    frameMs    = 0.0f;  // GPU wall time of last frame (CPU measured)
};

// Stats for a single mrtCommitScene() / Engine::commitScene() call (PRD §8 A1).
struct CommitStats {
    uint32_t blasRebuilt = 0;     // number of mesh BLASes rebuilt this commit
    bool     tlasRebuilt = false; // instance set repacked (TLAS rebuild)
    float    commitMs    = 0.0f;  // CPU wall time of the whole commit
};

} // namespace mrt
