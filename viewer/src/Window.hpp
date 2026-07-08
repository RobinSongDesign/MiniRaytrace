#pragma once
// GLFW window + Vulkan swapchain. Presents the engine's output image by blit.

// mrt_core links volk, not the Vulkan loader import lib (PRD §8 A4); the two
// cannot coexist in one binary (both define global vk* symbols). Since the
// viewer links mrt_core, it must route its own Vulkan calls through volk's
// already-loaded pointers too — include volk.h before GLFW's own vulkan.h
// (GLFW_INCLUDE_VULKAN) so the latter is a no-op past the header guard.
#include <volk.h>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <mrt/Engine.hpp>

#include <functional>
#include <vector>

namespace viewer {

class Window {
public:
    // GLFW must be initialised before Engine creation to query instance
    // extensions; use requiredInstanceExtensions() for EngineDesc.
    static std::vector<const char*> requiredInstanceExtensions();

    bool create(mrt::Engine& engine, uint32_t width, uint32_t height, const char* title);
    void destroy();
    ~Window() { destroy(); }

    GLFWwindow* glfw() const { return m_window; }
    bool shouldClose() const { return glfwWindowShouldClose(m_window); }

    // Blit the engine output image (GENERAL layout) to the swapchain, then
    // optionally record UI draw commands inside a render pass on top of it.
    void present(mrt::Engine& engine,
                 const std::function<void(VkCommandBuffer)>& recordUi = {});

    // Current framebuffer size (handles resize).
    void framebufferSize(uint32_t& w, uint32_t& h) const;

    // For ImGui initialisation.
    VkRenderPass renderPass() const { return m_renderPass; }
    uint32_t     imageCount() const { return uint32_t(m_swapImages.size()); }

private:
    bool createSwapchain();
    void destroySwapchain();
    void createRenderPass();

    mrt::Engine* m_engine = nullptr;
    GLFWwindow*  m_window = nullptr;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> m_swapImages;
    std::vector<VkImageView> m_swapViews;
    std::vector<VkFramebuffer> m_framebuffers;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    VkExtent2D m_extent{};
    VkSemaphore m_acquireSem = VK_NULL_HANDLE;
    VkSemaphore m_blitSem = VK_NULL_HANDLE;
    VkFence m_presentFence = VK_NULL_HANDLE;
    VkCommandPool m_cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
};

} // namespace viewer
