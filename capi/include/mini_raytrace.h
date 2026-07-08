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

/* Both formats: row-major, top-down (row 0 = top of the image — matches
 * Rhino's RenderWindow channel convention, no vertical flip needed). Alpha
 * is always 1.0 in v1 (no partial pixel coverage / alpha matte yet). */
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
    /* 0 (the zero-initialized default) = auto, prefer discrete GPU. N>0 = force
     * vkEnumeratePhysicalDevices index N-1 (PRD §8 A4 — Rhino's per-session GPU
     * preference on hybrid-graphics laptops). 1-based so a zeroed-out struct,
     * the common C/C# default, safely means "auto" rather than "device 0".
     * Device names/types are logged at Info level on every mrtCreateEngine call. */
    uint32_t gpuIndex1;
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

typedef enum mrtCameraProjection {
    MRT_CAMERA_PERSPECTIVE = 0,
    MRT_CAMERA_ORTHOGRAPHIC = 1,
} mrtCameraProjection;

/* Explicit asymmetric frustum + parallel projection (PRD §8 A2), matching
 * Rhino's ViewportInfo::GetFrustum() field-for-field: pass left/right/bottom/top
 * as returned by GetFrustum() divided by `near` for perspective, or as-is for
 * orthographic. Two-point perspective needs no separate flag: a level
 * `forward` with an asymmetric top/bottom already expresses it. */
typedef struct mrtCameraDescEx {
    size_t  structSize;
    int32_t projection;      /* mrtCameraProjection */
    float   position[3];
    float   forward[3];      /* unit */
    float   up[3];           /* up hint, orthonormalized against forward */
    float   left, right, bottom, top;
} mrtCameraDescEx;

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

typedef struct mrtCommitStats {
    size_t   structSize;
    uint32_t blasRebuilt;   /* meshes whose BVH was rebuilt this commit */
    int32_t  tlasRebuilt;   /* instance set was repacked                */
    float    commitMs;      /* CPU wall time of the commit              */
} mrtCommitStats;

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
/* Instances referencing a removed material fall back to the built-in default. */
MRT_API void          mrtRemoveMaterial(mrtEngine*, mrtMaterialId);
MRT_API mrtTextureId  mrtAddTexture(mrtEngine*, const mrtTextureDesc*);
/* Materials referencing a removed texture render that slot as a constant
 * (no texture); the slot itself is recycled by a later mrtAddTexture. */
MRT_API void          mrtRemoveTexture(mrtEngine*, mrtTextureId);

MRT_API mrtLightId    mrtAddLight(mrtEngine*, const mrtLightDesc*);
MRT_API void          mrtUpdateLight(mrtEngine*, mrtLightId, const mrtLightDesc*);
MRT_API void          mrtRemoveLight(mrtEngine*, mrtLightId);

/* hdri must be MRT_TEX_RGBA32F equirectangular; NULL clears the environment */
MRT_API void          mrtSetEnvironment(mrtEngine*, const mrtTextureDesc* hdri,
                                        float rotationRad, float intensity);
MRT_API void          mrtSetCamera(mrtEngine*, const mrtCameraDesc*);
MRT_API void          mrtSetCameraEx(mrtEngine*, const mrtCameraDescEx*);

/* Apply pending scene changes now (BLAS/TLAS rebuild, uploads, accumulation
 * reset) instead of waiting for the next mrtRenderFrame. Idempotent: a call
 * with no new dirty state is a cheap no-op. outStats may be NULL. */
MRT_API mrtResult     mrtCommitScene(mrtEngine*, mrtCommitStats* outStats);

/* ---- render --------------------------------------------------------------- */
MRT_API mrtResult mrtSetRenderSettings(mrtEngine*, const mrtRenderSettings*);
MRT_API mrtResult mrtRenderFrame(mrtEngine*, mrtFrameInfo* outInfo); /* blocking */
/* dst must hold width*height*4 (RGBA8) or width*height*16 (RGBA32F) bytes;
 * row-major top-down, alpha always 1.0 (see mrtPixelFormat). Reuses internal
 * staging buffers across calls — safe to call every frame. */
MRT_API mrtResult mrtReadFramebuffer(mrtEngine*, mrtPixelFormat, void* dst, size_t dstSize);
MRT_API void      mrtResetAccumulation(mrtEngine*);

/* ---- misc ------------------------------------------------------------------ */
typedef void (*mrtLogFn)(int level /*0=debug..3=error*/, const char* message, void* user);
MRT_API void        mrtSetLogCallback(mrtLogFn fn, void* user);
MRT_API const char* mrtResultToString(mrtResult r);
/* Human-readable detail for the most recent ErrorXxx result on this thread
 * (e.g. "No Vulkan loader found..."), valid until the next failing call.
 * Empty string if nothing has failed yet. Never NULL. */
MRT_API const char* mrtGetLastErrorMessage(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* MINI_RAYTRACE_H */
