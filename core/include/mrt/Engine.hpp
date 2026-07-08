#pragma once
// Engine: the public C++ facade tying GPU context, scene, BVH and renderer
// together. Headless by design — presentation is the consumer's concern
// (the viewer blits the output image; Rhino reads back pixels via the C API).

#include "Common.hpp"
#include "Scene.hpp"
#include "SceneTypes.hpp"

#include <memory>

// Forward-declare Vulkan handles so public headers don't force vulkan.h on consumers.
struct VkInstance_T;  struct VkPhysicalDevice_T; struct VkDevice_T;
struct VkQueue_T;     struct VkImage_T;

namespace mrt {

class GpuContext;
class Renderer;
class Denoiser;

struct EngineDesc {
    bool enableValidation     = false;
    bool needPresentSupport   = false;     // viewer sets true (adds swapchain device ext)
    const char* const* instanceExtensions = nullptr; // e.g. from glfwGetRequiredInstanceExtensions
    uint32_t instanceExtensionCount = 0;
    // -1 = auto (prefer discrete GPU); >=0 = force this vkEnumeratePhysicalDevices
    // index (PRD §8 A4 — Rhino's per-session GPU preference on hybrid-graphics
    // laptops). Query available devices first via a throwaway headless engine
    // or ship a "list devices" utility; there's no enumerate-without-creating
    // entry point in v1.
    int32_t gpuIndex = -1;
    RenderSettings settings;
};

// Layout for both formats (PRD §8 A7): row-major, top-down — row 0 is the
// top of the image, matching Rhino's RenderWindow channel convention, so
// integrations never need a vertical flip. Alpha is always 1.0 in v1 (no
// partial pixel coverage / alpha matte yet).
enum class ReadbackFormat : uint32_t {
    RGBA8_SRGB,    // tonemapped, display-ready (matches viewer output)
    RGBA32F_LINEAR // raw accumulated radiance (Rhino-managed color pipeline)
};

class Engine {
public:
    static Result create(const EngineDesc& desc, std::unique_ptr<Engine>& out);
    ~Engine();

    Scene& scene() { return m_scene; }

    Result setRenderSettings(const RenderSettings& s);
    const RenderSettings& renderSettings() const { return m_settings; }
    void   resetAccumulation();

    // Explicitly apply pending scene changes: BLAS/TLAS rebuilds, buffer
    // uploads, accumulation reset. Idempotent — a second call with no new
    // dirty state is a cheap no-op (PRD §8 A1). renderFrame() also calls this
    // internally, so callers that don't need commit stats may skip it.
    Result commitScene(CommitStats* outStats = nullptr);

    // Sync dirty scene state, trace one frame, resolve. Blocking (PRD §8).
    Result renderFrame(FrameInfo& outInfo);

    // Copy the current image to CPU memory. dst must hold width*height*4 (RGBA8)
    // or width*height*16 (RGBA32F) bytes.
    Result readFramebuffer(ReadbackFormat fmt, void* dst, size_t dstSize);

    // -- viewer interop (in-process C++ consumers only) ------------------
    VkInstance_T*       vkInstance() const;
    VkPhysicalDevice_T* vkPhysicalDevice() const;
    VkDevice_T*         vkDevice() const;
    VkQueue_T*          vkQueue() const;
    uint32_t            queueFamilyIndex() const;
    // Resolved output image (RGBA8_UNORM holding sRGB-encoded values),
    // left in VK_IMAGE_LAYOUT_GENERAL after renderFrame().
    VkImage_T*          outputImage() const;

private:
    Engine() = default;
    Result init(const EngineDesc& desc);

    std::unique_ptr<GpuContext> m_gpu;
    std::unique_ptr<Renderer>   m_renderer;
    std::unique_ptr<Denoiser>   m_denoiser;
    Scene          m_scene;
    RenderSettings m_settings;
};

} // namespace mrt
