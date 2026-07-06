#include "Window.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace viewer {

namespace {
void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[viewer] %s failed: %d\n", what, r);
        std::abort();
    }
}
} // namespace

std::vector<const char*> Window::requiredInstanceExtensions() {
    uint32_t count = 0;
    const char** exts = glfwGetRequiredInstanceExtensions(&count);
    return { exts, exts + count };
}

bool Window::create(mrt::Engine& engine, uint32_t width, uint32_t height, const char* title) {
    m_engine = &engine;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    m_window = glfwCreateWindow(int(width), int(height), title, nullptr, nullptr);
    if (!m_window) return false;

    VkInstance instance = reinterpret_cast<VkInstance>(engine.vkInstance());
    check(glfwCreateWindowSurface(instance, m_window, nullptr, &m_surface),
          "glfwCreateWindowSurface");

    VkDevice device = reinterpret_cast<VkDevice>(engine.vkDevice());
    VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    check(vkCreateSemaphore(device, &sci, nullptr, &m_acquireSem), "semaphore");
    check(vkCreateSemaphore(device, &sci, nullptr, &m_blitSem), "semaphore");
    VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    check(vkCreateFence(device, &fci, nullptr, &m_presentFence), "fence");

    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = engine.queueFamilyIndex();
    check(vkCreateCommandPool(device, &pci, nullptr, &m_cmdPool), "command pool");
    VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = m_cmdPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device, &cai, &m_cmd), "command buffer");

    return createSwapchain();
}

bool Window::createSwapchain() {
    VkPhysicalDevice phys = reinterpret_cast<VkPhysicalDevice>(m_engine->vkPhysicalDevice());
    VkDevice device = reinterpret_cast<VkDevice>(m_engine->vkDevice());

    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(phys, m_engine->queueFamilyIndex(), m_surface, &supported);
    if (!supported) {
        fprintf(stderr, "[viewer] queue family lacks present support\n");
        return false;
    }

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, m_surface, &caps);

    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(m_window, &fbw, &fbh);
    m_extent.width  = std::clamp(uint32_t(fbw), caps.minImageExtent.width, caps.maxImageExtent.width);
    m_extent.height = std::clamp(uint32_t(fbh), caps.minImageExtent.height, caps.maxImageExtent.height);

    // UNORM (not sRGB) target: the resolve shader already writes sRGB-encoded
    // values, presenting them untouched is correct (PRD §3.4).
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, m_surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, m_surface, &fmtCount, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM)
            { chosen = f; break; }

    VkSwapchainCreateInfoKHR ci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = m_surface;
    ci.minImageCount = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount > 0) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = m_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR; // vsync, universally supported
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = m_swapchain;

    VkSwapchainKHR newChain = VK_NULL_HANDLE;
    check(vkCreateSwapchainKHR(device, &ci, nullptr, &newChain), "vkCreateSwapchainKHR");
    if (m_swapchain) vkDestroySwapchainKHR(device, m_swapchain, nullptr);
    m_swapchain = newChain;

    uint32_t imgCount = 0;
    vkGetSwapchainImagesKHR(device, m_swapchain, &imgCount, nullptr);
    m_swapImages.resize(imgCount);
    vkGetSwapchainImagesKHR(device, m_swapchain, &imgCount, m_swapImages.data());

    // (Re)create views + framebuffers for the UI render pass.
    m_format = chosen.format;
    if (!m_renderPass) createRenderPass();

    for (VkFramebuffer fb : m_framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
    for (VkImageView v : m_swapViews) vkDestroyImageView(device, v, nullptr);
    m_framebuffers.clear();
    m_swapViews.clear();

    for (VkImage img : m_swapImages) {
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = m_format;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageView view;
        check(vkCreateImageView(device, &vi, nullptr, &view), "swapchain view");
        m_swapViews.push_back(view);

        VkFramebufferCreateInfo fi{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = m_renderPass;
        fi.attachmentCount = 1;
        fi.pAttachments = &view;
        fi.width = m_extent.width;
        fi.height = m_extent.height;
        fi.layers = 1;
        VkFramebuffer fb;
        check(vkCreateFramebuffer(device, &fi, nullptr, &fb), "framebuffer");
        m_framebuffers.push_back(fb);
    }
    return true;
}

void Window::createRenderPass() {
    VkDevice device = reinterpret_cast<VkDevice>(m_engine->vkDevice());

    // Loads the blitted image and draws UI on top; handles the
    // TRANSFER_DST -> PRESENT layout transition.
    VkAttachmentDescription att{};
    att.format = m_format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;

    VkRenderPassCreateInfo ci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.attachmentCount = 1;
    ci.pAttachments = &att;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;
    check(vkCreateRenderPass(device, &ci, nullptr, &m_renderPass), "render pass");
}

void Window::destroySwapchain() {
    VkDevice device = reinterpret_cast<VkDevice>(m_engine->vkDevice());
    for (VkFramebuffer fb : m_framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
    for (VkImageView v : m_swapViews) vkDestroyImageView(device, v, nullptr);
    m_framebuffers.clear();
    m_swapViews.clear();
    if (m_swapchain) vkDestroySwapchainKHR(device, m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;
}

void Window::destroy() {
    if (!m_engine || !m_window) return;
    VkDevice device = reinterpret_cast<VkDevice>(m_engine->vkDevice());
    VkInstance instance = reinterpret_cast<VkInstance>(m_engine->vkInstance());
    vkDeviceWaitIdle(device);
    destroySwapchain();
    if (m_renderPass) vkDestroyRenderPass(device, m_renderPass, nullptr);
    m_renderPass = VK_NULL_HANDLE;
    if (m_cmdPool) vkDestroyCommandPool(device, m_cmdPool, nullptr);
    if (m_acquireSem) vkDestroySemaphore(device, m_acquireSem, nullptr);
    if (m_blitSem) vkDestroySemaphore(device, m_blitSem, nullptr);
    if (m_presentFence) vkDestroyFence(device, m_presentFence, nullptr);
    if (m_surface) vkDestroySurfaceKHR(instance, m_surface, nullptr);
    glfwDestroyWindow(m_window);
    m_window = nullptr;
}

void Window::framebufferSize(uint32_t& w, uint32_t& h) const {
    int iw = 0, ih = 0;
    glfwGetFramebufferSize(m_window, &iw, &ih);
    w = uint32_t(std::max(iw, 1));
    h = uint32_t(std::max(ih, 1));
}

void Window::present(mrt::Engine& engine,
                     const std::function<void(VkCommandBuffer)>& recordUi) {
    VkDevice device = reinterpret_cast<VkDevice>(engine.vkDevice());
    VkQueue queue = reinterpret_cast<VkQueue>(engine.vkQueue());

    uint32_t imageIndex = 0;
    VkResult r = vkAcquireNextImageKHR(device, m_swapchain, UINT64_MAX,
                                       m_acquireSem, VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(device);
        createSwapchain();
        return;
    }
    check(r, "vkAcquireNextImageKHR");

    VkImage src = reinterpret_cast<VkImage>(engine.outputImage());
    VkImage dst = m_swapImages[imageIndex];
    const auto& settings = engine.renderSettings();

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(m_cmd, 0);
    vkBeginCommandBuffer(m_cmd, &bi);

    auto barrier = [&](VkImage img, VkImageLayout from, VkImageLayout to,
                       VkAccessFlags srcA, VkAccessFlags dstA,
                       VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(m_cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    barrier(src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageBlit blit{};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[1] = { int32_t(settings.width), int32_t(settings.height), 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[1] = { int32_t(m_extent.width), int32_t(m_extent.height), 1 };
    vkCmdBlitImage(m_cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    if (recordUi) {
        // Render pass handles TRANSFER_DST -> PRESENT and draws UI on top.
        VkRenderPassBeginInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rp.renderPass = m_renderPass;
        rp.framebuffer = m_framebuffers[imageIndex];
        rp.renderArea.extent = m_extent;
        vkCmdBeginRenderPass(m_cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        recordUi(m_cmd);
        vkCmdEndRenderPass(m_cmd);
    } else {
        barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    barrier(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkEndCommandBuffer(m_cmd);

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &m_acquireSem;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &m_cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &m_blitSem;
    check(vkQueueSubmit(queue, 1, &si, m_presentFence), "vkQueueSubmit (blit)");
    check(vkWaitForFences(device, 1, &m_presentFence, VK_TRUE, UINT64_MAX), "fence wait");
    vkResetFences(device, 1, &m_presentFence);

    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &m_blitSem;
    pi.swapchainCount = 1;
    pi.pSwapchains = &m_swapchain;
    pi.pImageIndices = &imageIndex;
    r = vkQueuePresentKHR(queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(device);
        createSwapchain();
    }
}

} // namespace viewer
