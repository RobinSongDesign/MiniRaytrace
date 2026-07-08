// C ABI implementation: thin translation layer over mrt::Engine.
// No exceptions escape; descriptor structs are converted field by field.

#include "mini_raytrace.h"

#include <mrt/Engine.hpp>

#include <cstring>

namespace {

struct EngineWrapper {
    std::unique_ptr<mrt::Engine> engine;
};

mrt::Engine* unwrap(mrtEngine* e) {
    return e ? reinterpret_cast<EngineWrapper*>(e)->engine.get() : nullptr;
}

mrtResult toC(mrt::Result r) { return static_cast<mrtResult>(r); }

// Guard: returns fallback if the struct is null or older than expected.
template <typename T>
bool validStruct(const T* s) { return s && s->structSize >= sizeof(T); }

mrt::MeshDesc convert(const mrtMeshDesc& d) {
    mrt::MeshDesc m;
    m.vertexCount = d.vertexCount;
    m.indexCount = d.indexCount;
    m.positions = d.positions;
    m.normals = d.normals;
    m.uvs = d.uvs;
    m.indices = d.indices;
    return m;
}

mrt::MaterialDesc convert(const mrtMaterialDesc& d) {
    mrt::MaterialDesc m;
    std::memcpy(m.baseColor, d.baseColor, sizeof(m.baseColor));
    m.opacity = d.opacity;
    m.roughness = d.roughness;
    m.metallic = d.metallic;
    m.transmission = d.transmission;
    m.ior = d.ior;
    std::memcpy(m.emission, d.emission, sizeof(m.emission));
    m.baseColorTex = d.baseColorTex;
    m.roughnessTex = d.roughnessTex;
    m.metallicTex = d.metallicTex;
    m.normalTex = d.normalTex;
    m.emissionTex = d.emissionTex;
    return m;
}

mrt::TextureDesc convert(const mrtTextureDesc& d) {
    mrt::TextureDesc t;
    t.width = d.width;
    t.height = d.height;
    t.format = static_cast<mrt::TextureFormat>(d.format);
    t.pixels = d.pixels;
    t.generateMips = d.generateMips != 0;
    return t;
}

mrt::LightDesc convert(const mrtLightDesc& d) {
    mrt::LightDesc l;
    l.type = static_cast<mrt::LightType>(d.type + 0); // enums align: Sun/Point/Rect
    std::memcpy(l.direction, d.direction, sizeof(l.direction));
    std::memcpy(l.position, d.position, sizeof(l.position));
    std::memcpy(l.radiance, d.radiance, sizeof(l.radiance));
    l.angularRadius = d.angularRadius;
    std::memcpy(l.corner, d.corner, sizeof(l.corner));
    std::memcpy(l.edge0, d.edge0, sizeof(l.edge0));
    std::memcpy(l.edge1, d.edge1, sizeof(l.edge1));
    l.twoSided = d.twoSided != 0;
    return l;
}

mrt::RenderSettings convert(const mrtRenderSettings& d) {
    mrt::RenderSettings s;
    s.width = d.width;
    s.height = d.height;
    s.maxBounces = d.maxBounces;
    s.sppLimit = d.sppLimit;
    s.exposureEV = d.exposureEV;
    s.fireflyClamp = d.fireflyClamp;
    s.tonemap = static_cast<mrt::TonemapMode>(d.tonemap);
    s.denoise = d.denoise != 0;
    return s;
}

} // namespace

extern "C" {

MRT_API mrtResult mrtCreateEngine(const mrtEngineDesc* desc, mrtEngine** outEngine) {
    if (!validStruct(desc) || !outEngine) return MRT_ERROR_INVALID_ARGUMENT;
    try {
        mrt::EngineDesc ed;
        ed.enableValidation = desc->enableValidation != 0;
        ed.needPresentSupport = false; // headless: Rhino reads pixels back
        // 1-based at the C boundary so a zero-initialized struct means "auto"
        // (see mrtEngineDesc::gpuIndex1); the C++ side is 0-based, -1 = auto.
        ed.gpuIndex = (desc->gpuIndex1 > 0) ? static_cast<int32_t>(desc->gpuIndex1 - 1) : -1;
        if (desc->width > 0 && desc->height > 0) {
            ed.settings.width = desc->width;
            ed.settings.height = desc->height;
        }
        auto* wrapper = new EngineWrapper();
        const mrt::Result r = mrt::Engine::create(ed, wrapper->engine);
        if (r != mrt::Result::Success) {
            delete wrapper;
            return toC(r);
        }
        *outEngine = reinterpret_cast<mrtEngine*>(wrapper);
        return MRT_SUCCESS;
    } catch (const std::bad_alloc&) {
        return MRT_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return MRT_ERROR_UNKNOWN;
    }
}

MRT_API void mrtDestroyEngine(mrtEngine* engine) {
    delete reinterpret_cast<EngineWrapper*>(engine);
}

/* ---- scene ----------------------------------------------------------------- */

MRT_API mrtMeshId mrtAddMesh(mrtEngine* e, const mrtMeshDesc* d) {
    mrt::Engine* eng = unwrap(e);
    if (!eng || !validStruct(d)) return MRT_INVALID_ID;
    return eng->scene().addMesh(convert(*d));
}

MRT_API void mrtUpdateMesh(mrtEngine* e, mrtMeshId id, const mrtMeshDesc* d) {
    mrt::Engine* eng = unwrap(e);
    if (eng && validStruct(d)) eng->scene().updateMesh(id, convert(*d));
}

MRT_API void mrtRemoveMesh(mrtEngine* e, mrtMeshId id) {
    if (mrt::Engine* eng = unwrap(e)) eng->scene().removeMesh(id);
}

MRT_API mrtInstanceId mrtAddInstance(mrtEngine* e, mrtMeshId mesh,
                                     const float* xform3x4, mrtMaterialId mat) {
    mrt::Engine* eng = unwrap(e);
    if (!eng) return MRT_INVALID_ID;
    return eng->scene().addInstance(mesh, xform3x4, mat);
}

MRT_API void mrtSetInstanceTransform(mrtEngine* e, mrtInstanceId id, const float* xform3x4) {
    if (mrt::Engine* eng = unwrap(e)) eng->scene().setInstanceTransform(id, xform3x4);
}

MRT_API void mrtRemoveInstance(mrtEngine* e, mrtInstanceId id) {
    if (mrt::Engine* eng = unwrap(e)) eng->scene().removeInstance(id);
}

MRT_API mrtMaterialId mrtAddMaterial(mrtEngine* e, const mrtMaterialDesc* d) {
    mrt::Engine* eng = unwrap(e);
    if (!eng || !validStruct(d)) return MRT_INVALID_ID;
    return eng->scene().addMaterial(convert(*d));
}

MRT_API void mrtUpdateMaterial(mrtEngine* e, mrtMaterialId id, const mrtMaterialDesc* d) {
    mrt::Engine* eng = unwrap(e);
    if (eng && validStruct(d)) eng->scene().updateMaterial(id, convert(*d));
}

MRT_API void mrtRemoveMaterial(mrtEngine* e, mrtMaterialId id) {
    if (mrt::Engine* eng = unwrap(e)) eng->scene().removeMaterial(id);
}

MRT_API mrtTextureId mrtAddTexture(mrtEngine* e, const mrtTextureDesc* d) {
    mrt::Engine* eng = unwrap(e);
    if (!eng || !validStruct(d)) return MRT_INVALID_ID;
    return eng->scene().addTexture(convert(*d));
}

MRT_API void mrtRemoveTexture(mrtEngine* e, mrtTextureId id) {
    if (mrt::Engine* eng = unwrap(e)) eng->scene().removeTexture(id);
}

MRT_API mrtLightId mrtAddLight(mrtEngine* e, const mrtLightDesc* d) {
    mrt::Engine* eng = unwrap(e);
    if (!eng || !validStruct(d)) return MRT_INVALID_ID;
    return eng->scene().addLight(convert(*d));
}

MRT_API void mrtUpdateLight(mrtEngine* e, mrtLightId id, const mrtLightDesc* d) {
    mrt::Engine* eng = unwrap(e);
    if (eng && validStruct(d)) eng->scene().updateLight(id, convert(*d));
}

MRT_API void mrtRemoveLight(mrtEngine* e, mrtLightId id) {
    if (mrt::Engine* eng = unwrap(e)) eng->scene().removeLight(id);
}

MRT_API void mrtSetEnvironment(mrtEngine* e, const mrtTextureDesc* hdri,
                               float rotationRad, float intensity) {
    mrt::Engine* eng = unwrap(e);
    if (!eng) return;
    if (!hdri) {
        eng->scene().clearEnvironment();
        return;
    }
    if (validStruct(hdri))
        eng->scene().setEnvironment(convert(*hdri), rotationRad, intensity);
}

MRT_API void mrtSetCamera(mrtEngine* e, const mrtCameraDesc* d) {
    mrt::Engine* eng = unwrap(e);
    if (!eng || !validStruct(d)) return;
    mrt::CameraDesc c;
    std::memcpy(c.position, d->position, sizeof(c.position));
    std::memcpy(c.target, d->target, sizeof(c.target));
    std::memcpy(c.up, d->up, sizeof(c.up));
    c.fovYDeg = d->fovYDeg;
    eng->scene().setCamera(c);
}

MRT_API void mrtSetCameraEx(mrtEngine* e, const mrtCameraDescEx* d) {
    mrt::Engine* eng = unwrap(e);
    if (!eng || !validStruct(d)) return;
    mrt::CameraDescEx c;
    c.projection = static_cast<mrt::CameraProjection>(d->projection);
    std::memcpy(c.position, d->position, sizeof(c.position));
    std::memcpy(c.forward, d->forward, sizeof(c.forward));
    std::memcpy(c.up, d->up, sizeof(c.up));
    c.left = d->left; c.right = d->right; c.bottom = d->bottom; c.top = d->top;
    eng->scene().setCameraEx(c);
}

MRT_API mrtResult mrtCommitScene(mrtEngine* e, mrtCommitStats* outStats) {
    mrt::Engine* eng = unwrap(e);
    if (!eng) return MRT_ERROR_INVALID_HANDLE;
    try {
        mrt::CommitStats stats;
        const mrt::Result r = eng->commitScene(&stats);
        if (outStats && outStats->structSize >= sizeof(mrtCommitStats)) {
            outStats->blasRebuilt = stats.blasRebuilt;
            outStats->tlasRebuilt = stats.tlasRebuilt ? 1 : 0;
            outStats->commitMs = stats.commitMs;
        }
        return toC(r);
    } catch (...) {
        return MRT_ERROR_UNKNOWN;
    }
}

/* ---- render ----------------------------------------------------------------- */

MRT_API mrtResult mrtSetRenderSettings(mrtEngine* e, const mrtRenderSettings* d) {
    mrt::Engine* eng = unwrap(e);
    if (!eng || !validStruct(d)) return MRT_ERROR_INVALID_ARGUMENT;
    try {
        return toC(eng->setRenderSettings(convert(*d)));
    } catch (...) {
        return MRT_ERROR_UNKNOWN;
    }
}

MRT_API mrtResult mrtRenderFrame(mrtEngine* e, mrtFrameInfo* outInfo) {
    mrt::Engine* eng = unwrap(e);
    if (!eng) return MRT_ERROR_INVALID_HANDLE;
    try {
        mrt::FrameInfo info;
        const mrt::Result r = eng->renderFrame(info);
        if (outInfo && outInfo->structSize >= sizeof(mrtFrameInfo)) {
            outInfo->spp = info.spp;
            outInfo->converged = info.converged ? 1 : 0;
            outInfo->denoised = info.denoised ? 1 : 0;
            outInfo->frameMs = info.frameMs;
        }
        return toC(r);
    } catch (...) {
        return MRT_ERROR_UNKNOWN;
    }
}

MRT_API mrtResult mrtReadFramebuffer(mrtEngine* e, mrtPixelFormat fmt,
                                     void* dst, size_t dstSize) {
    mrt::Engine* eng = unwrap(e);
    if (!eng || !dst) return MRT_ERROR_INVALID_ARGUMENT;
    try {
        return toC(eng->readFramebuffer(static_cast<mrt::ReadbackFormat>(fmt), dst, dstSize));
    } catch (...) {
        return MRT_ERROR_UNKNOWN;
    }
}

MRT_API void mrtResetAccumulation(mrtEngine* e) {
    if (mrt::Engine* eng = unwrap(e)) eng->resetAccumulation();
}

/* ---- misc -------------------------------------------------------------------- */

MRT_API void mrtSetLogCallback(mrtLogFn fn, void* user) {
    if (!fn) {
        mrt::setLogCallback(nullptr);
        return;
    }
    mrt::setLogCallback([fn, user](mrt::LogLevel level, const std::string& msg) {
        fn(static_cast<int>(level), msg.c_str(), user);
    });
}

MRT_API const char* mrtResultToString(mrtResult r) {
    return mrt::toString(static_cast<mrt::Result>(r));
}

MRT_API const char* mrtGetLastErrorMessage(void) {
    return mrt::lastErrorMessage().c_str();
}

} // extern "C"
