#pragma once
// CPU mirrors of the std430 GLSL structures (see core/shaders/common.glsl).
// Field order, sizes and alignment must stay byte-identical with the shaders.

#include "mrt/MathTypes.hpp"
#include <cstdint>

namespace mrt {

// Descriptor set 0 binding indices (shared by all pipelines).
enum Binding : uint32_t {
    BindGlobals      = 0,
    BindAccum        = 1,
    BindAovAlbedo    = 2,
    BindAovNormal    = 3,
    BindDenoised     = 4,
    BindNodes        = 5,
    BindTriangles    = 6,
    BindPositions    = 7,
    BindNormals      = 8,
    BindUvs          = 9,
    BindTangents     = 10,
    BindInstances    = 11,
    BindMaterials    = 12,
    BindLights       = 13,
    BindEnvCdf       = 14,
    BindEnvMap       = 15,
    BindTextureArray = 16,
    BindOutputImage  = 17,
    BindingCount     = 18,
};

// Flag bits in GpuGlobals.counts.w
enum GlobalFlags : uint32_t {
    FlagHasEnv      = 1u << 0,
    FlagUseDenoised = 1u << 1,
};

struct GpuGlobals {                  // std430 buffer, binding 0
    vec4  camPos;                    // xyz = position
    vec4  camRight;                  // xyz = right * tan(fovY/2) * aspect
    vec4  camUp;                     // xyz = up * tan(fovY/2)
    vec4  camForward;                // xyz = forward (unit)
    uint32_t width, height, frameIndex, maxBounces;
    uint32_t lightCount, envCdfWidth, envCdfHeight, flags;
    float envRotation, envIntensity, envIntegral, exposure;
    float fireflyClamp; uint32_t tonemapMode, tlasRoot, debugView;
};
static_assert(sizeof(GpuGlobals) == 4 * 16 + 4 * 16, "GpuGlobals layout drift");

struct GpuInstance {                 // 144 bytes
    mat4 objectToWorld;
    mat4 worldToObject;
    uint32_t blasRoot;               // node index of the BLAS root (global array)
    uint32_t materialIndex;          // index into materials buffer
    uint32_t _pad0, _pad1;
};
static_assert(sizeof(GpuInstance) == 144, "GpuInstance layout drift");

struct GpuMaterial {                 // 80 bytes
    vec4 baseColorOpacity;           // rgb + opacity
    vec4 emissionRoughness;          // rgb emission + roughness
    vec4 params;                     // metallic, transmission, ior, pad
    int32_t baseColorTex, roughnessTex, metallicTex, normalTex;
    int32_t emissionTex, _pad0, _pad1, _pad2;
};
static_assert(sizeof(GpuMaterial) == 80, "GpuMaterial layout drift");

// Light types in meta.x — keep in sync with lights.glsl.
enum GpuLightType : int32_t {
    GpuLightEnv = 0, GpuLightSun = 1, GpuLightPoint = 2, GpuLightRect = 3,
};

struct GpuLight {                    // 80 bytes
    vec4 p0; // sun: dir(xyz)+cosAngularRadius | point: pos | rect: corner+area
    vec4 p1; // sun: radiance | point: intensity | rect: edge0
    vec4 p2; // rect: edge1
    vec4 p3; // rect: radiance + twoSided(w)
    int32_t type, _pad0, _pad1, _pad2;
};
static_assert(sizeof(GpuLight) == 80, "GpuLight layout drift");

struct GpuTriangle {                 // uvec4: global vertex indices + pad
    uint32_t i0, i1, i2, _pad;
};

} // namespace mrt
