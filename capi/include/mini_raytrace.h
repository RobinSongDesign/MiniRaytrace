/*
 * mini_raytrace.h — stable C ABI for the MiniRaytrace core (PRD §8).
 *
 * Designed for P/Invoke from the Rhino C# plugin. Conventions:
 *   - opaque handles + error codes, no exceptions cross this boundary
 *   - all pointers are borrowed for the duration of the call (deep copy inside)
 *   - all calls must come from a single thread
 *   - every struct begins with structSize for forward compatibility
 */
#ifndef MINI_RAYTRACE_H
#define MINI_RAYTRACE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
  #define MRT_API __declspec(dllexport)
#else
  #define MRT_API __attribute__((visibility("default")))
#endif

typedef struct mrtEngine mrtEngine;

typedef enum mrtResult {
    MRT_SUCCESS = 0,
    MRT_ERROR_UNKNOWN,
    MRT_ERROR_VULKAN_INIT,
    MRT_ERROR_OUT_OF_MEMORY,
    MRT_ERROR_INVALID_ARGUMENT,
    MRT_ERROR_INVALID_HANDLE,
    MRT_ERROR_SHADER_LOAD,
    MRT_ERROR_DEVICE_LOST,
} mrtResult;

typedef uint32_t mrtMeshId;
typedef uint32_t mrtInstanceId;
typedef uint32_t mrtMaterialId;
typedef uint32_t mrtTextureId;
typedef uint32_t mrtLightId;
#define MRT_INVALID_ID 0u

typedef enum mrtPixelFormat {
    MRT_PIXEL_RGBA8_SRGB = 0,   /* tonemapped, display-ready, 4 bytes/px  */
    MRT_PIXEL_RGBA32F_LINEAR,   /* raw averaged radiance, 16 bytes/px      */
} mrtPixelFormat;

typedef enum mrtTextureFormat {
    MRT_TEX_RGBA8_SRGB = 0,
    MRT_TEX_RGBA8_UNORM,
    MRT_TEX_RGBA32F,
} mrtTextureFormat;

typedef enum mrtLightType {
    MRT_LIGHT_SUN = 0,
    MRT_LIGHT_POINT,
    MRT_LIGHT_RECT,
} mrtLightType;

typedef enum mrtTonemap {
    MRT_TONEMAP_LINEAR = 0,
    MRT_TONEMAP_ACES,
} mrtTonemap;

/* ---- descriptor structs ------------------------------------------------- */

typedef struct mrtEngineDesc {
    size_t   structSize;        /* = sizeof(mrtEngineDesc) */
    int32_t  enableValidation;
    uint32_t width, height;
} mrtEngineDesc;

typedef struct mrtMeshDesc {
    size_t          structSize;
    uint32_t        vertexCount;
    uint32_t        indexCount;     /* multiple of 3 */
    const float*    positions;      /* xyz per vertex, required */
    const float*    normals;        /* xyz, optional */
    const float*    uvs;            /* uv, optional */
    const uint32_t* indices;        /* required */
} mrtMeshDesc;

typedef struct mrtMaterialDesc {
    size_t  structSize;
    float   baseColor[3];
    float   opacity;
    float   roughness;
    float   metallic;
    float   transmission;
    float   ior;
    float   emission[3];
    mrtTextureId baseColorTex;      /* MRT_INVALID_ID = constant */
    mrtTextureId roughnessTex;
    mrtTextureId metallicTex;
    mrtTextureId normalTex;
    mrtTextureId emissionTex;
} mrtMaterialDesc;

typedef struct mrtTextureDesc {
    size_t           structSize;
    uint32_t         width, height;
    mrtTextureFormat format;
    const void*      pixels;        /* tightly packed RGBA */
    int32_t          generateMips;
} mrtTextureDesc;

typedef struct mrtLightDesc {
    size_t       structSize;
    mrtLightType type;
    float direction[3];             /* sun: towards the light */
    float position[3];              /* point */
    float radiance[3];              /* sun/rect: radiance; point: intensity W/sr */
    float angularRadius;            /* sun, radians */
    float corner[3], edge0[3], edge1[3]; /* rect */
    int32_t twoSided;               /* rect */
} mrtLightDesc;

typedef struct mrtCameraDesc {
    size_t structSize;
    float position[3];
    float target[3];
    float up[3];
    float fovYDeg;
} mrtCameraDesc;

typedef struct mrtRenderSettings {
    size_t     structSize;
    uint32_t   width, height;
    uint32_t   maxBounces;          /* default 8 */
    uint32_t   sppLimit;            /* default 1024 */
    float      exposureEV;          /* default 0 */
    float      fireflyClamp;        /* 0 = off, default 50 */
    mrtTonemap tonemap;             /* Rhino integration: use LINEAR + RGBA32F readback */
    int32_t    denoise;             /* no-op without OIDN build */
} mrtRenderSettings;

typedef struct mrtFrameInfo {
    size_t   structSize;
    uint32_t spp;
    int32_t  converged;
    int32_t  denoised;
    float    frameMs;
} mrtFrameInfo;

/* ---- lifecycle ----------------------------------------------------------- */
MRT_API mrtResult mrtCreateEngine(const mrtEngineDesc* desc, mrtEngine** outEngine);
MRT_API void      mrtDestroyEngine(mrtEngine* engine);

/* ---- scene: incremental, mirrors the Rhino ChangeQueue (PRD §9) ---------- */
MRT_API mrtMeshId     mrtAddMesh(mrtEngine*, const mrtMeshDesc*);
MRT_API void          mrtUpdateMesh(mrtEngine*, mrtMeshId, const mrtMeshDesc*);
MRT_API void          mrtRemoveMesh(mrtEngine*, mrtMeshId);

/* xform3x4: row-major 3x4 (rows of [R|t]); NULL = identity */
MRT_API mrtInstanceId mrtAddInstance(mrtEngine*, mrtMeshId, const float* xform3x4, mrtMaterialId);
MRT_API void          mrtSetInstanceTransform(mrtEngine*, mrtInstanceId, const float* xform3x4);
MRT_API void          mrtRemoveInstance(mrtEngine*, mrtInstanceId);

MRT_API mrtMaterialId mrtAddMaterial(mrtEngine*, const mrtMaterialDesc*);
MRT_API void          mrtUpdateMaterial(mrtEngine*, mrtMaterialId, const mrtMaterialDesc*);
MRT_API mrtTextureId  mrtAddTexture(mrtEngine*, const mrtTextureDesc*);

MRT_API mrtLightId    mrtAddLight(mrtEngine*, const mrtLightDesc*);
MRT_API void          mrtUpdateLight(mrtEngine*, mrtLightId, const mrtLightDesc*);
MRT_API void          mrtRemoveLight(mrtEngine*, mrtLightId);

/* hdri must be MRT_TEX_RGBA32F equirectangular; NULL clears the environment */
MRT_API void          mrtSetEnvironment(mrtEngine*, const mrtTextureDesc* hdri,
                                        float rotationRad, float intensity);
MRT_API void          mrtSetCamera(mrtEngine*, const mrtCameraDesc*);

/* ---- render --------------------------------------------------------------- */
MRT_API mrtResult mrtSetRenderSettings(mrtEngine*, const mrtRenderSettings*);
MRT_API mrtResult mrtRenderFrame(mrtEngine*, mrtFrameInfo* outInfo); /* blocking */
MRT_API mrtResult mrtReadFramebuffer(mrtEngine*, mrtPixelFormat, void* dst, size_t dstSize);
MRT_API void      mrtResetAccumulation(mrtEngine*);

/* ---- misc ------------------------------------------------------------------ */
typedef void (*mrtLogFn)(int level /*0=debug..3=error*/, const char* message, void* user);
MRT_API void        mrtSetLogCallback(mrtLogFn fn, void* user);
MRT_API const char* mrtResultToString(mrtResult r);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* MINI_RAYTRACE_H */
