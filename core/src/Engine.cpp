// Engine facade: wires GpuContext + Renderer + Denoiser together.

#include "mrt/Engine.hpp"
#include "gpu/GpuContext.hpp"
#include "render/Renderer.hpp"
#include "post/Denoiser.hpp"

namespace mrt {

Engine::~Engine() = default;

Result Engine::create(const EngineDesc& desc, std::unique_ptr<Engine>& out) {
    std::unique_ptr<Engine> e(new Engine());
    const Result r = e->init(desc);
    if (r == Result::Success) out = std::move(e);
    return r;
}

Result Engine::init(const EngineDesc& desc) {
    m_settings = desc.settings;

    GpuContextDesc gd;
    gd.enableValidation = desc.enableValidation;
    gd.needPresentSupport = desc.needPresentSupport;
    for (uint32_t i = 0; i < desc.instanceExtensionCount; ++i)
        gd.instanceExtensions.push_back(desc.instanceExtensions[i]);

    m_gpu = std::make_unique<GpuContext>();
    if (Result r = m_gpu->init(gd); r != Result::Success) return r;

    m_denoiser = std::make_unique<Denoiser>();
    m_renderer = std::make_unique<Renderer>(*m_gpu, m_denoiser.get());
    return m_renderer->init(m_settings);
}

Result Engine::setRenderSettings(const RenderSettings& s) {
    m_settings = s;
    return m_renderer->setSettings(s);
}

void Engine::resetAccumulation() { m_renderer->resetAccumulation(); }

Result Engine::renderFrame(FrameInfo& outInfo) {
    // Camera / env-param changes need no GPU repack, just an accumulation
    // restart (both are read from fresh globals every frame).
    const uint32_t lightweight = DirtyCamera | DirtyEnvParams;
    if (m_scene.dirty() != DirtyNone && (m_scene.dirty() & ~lightweight) == 0) {
        m_scene.clearDirty();
        m_renderer->resetAccumulation();
    }
    return m_renderer->renderFrame(m_scene, outInfo);
}

Result Engine::readFramebuffer(ReadbackFormat fmt, void* dst, size_t dstSize) {
    return m_renderer->readback(fmt, dst, dstSize);
}

VkInstance_T*       Engine::vkInstance() const       { return m_gpu->instance(); }
VkPhysicalDevice_T* Engine::vkPhysicalDevice() const { return m_gpu->physicalDevice(); }
VkDevice_T*         Engine::vkDevice() const         { return m_gpu->device(); }
VkQueue_T*          Engine::vkQueue() const          { return m_gpu->queue(); }
uint32_t            Engine::queueFamilyIndex() const { return m_gpu->queueFamily(); }
VkImage_T*          Engine::outputImage() const      { return m_renderer->output().handle(); }

} // namespace mrt
