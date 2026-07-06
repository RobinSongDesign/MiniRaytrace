// Vulkan context implementation. Single queue, blocking submits (PRD §5:
// "simple correct first"). VMA does all memory management.

#define VMA_IMPLEMENTATION
#include "GpuContext.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace mrt {

// ---------------------------------------------------------------- logging --
namespace {
std::mutex g_logMutex;
LogFn g_logFn;
} // namespace

void setLogCallback(LogFn fn) { std::lock_guard l(g_logMutex); g_logFn = std::move(fn); }

void log(LogLevel level, const std::string& msg) {
    std::lock_guard l(g_logMutex);
    if (g_logFn) { g_logFn(level, msg); return; }
    fprintf(stderr, "[mrt] %s\n", msg.c_str());
}

const char* toString(Result r) {
    switch (r) {
        case Result::Success:              return "Success";
        case Result::ErrorVulkanInit:      return "ErrorVulkanInit";
        case Result::ErrorOutOfMemory:     return "ErrorOutOfMemory";
        case Result::ErrorInvalidArgument: return "ErrorInvalidArgument";
        case Result::ErrorInvalidHandle:   return "ErrorInvalidHandle";
        case Result::ErrorShaderLoad:      return "ErrorShaderLoad";
        case Result::ErrorDeviceLost:      return "ErrorDeviceLost";
        default:                           return "ErrorUnknown";
    }
}

// ----------------------------------------------------------------- Buffer --
void Buffer::create(GpuContext& gpu, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible) {
    destroy();
    m_gpu = &gpu;
    m_size = size;

    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (hostVisible) {
        // RANDOM (not SEQUENTIAL_WRITE) guarantees a host-visible, mappable
        // allocation that is also safe to read back from (readback paths).
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                   VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    VmaAllocationInfo info{};
    MRT_VK_CHECK(vmaCreateBuffer(gpu.allocator(), &bi, &ai, &m_buffer, &m_alloc, &info));
    m_mapped = hostVisible ? info.pMappedData : nullptr;
}

void Buffer::destroy() {
    if (m_buffer && m_gpu)
        vmaDestroyBuffer(m_gpu->allocator(), m_buffer, m_alloc);
    m_buffer = VK_NULL_HANDLE; m_alloc = VK_NULL_HANDLE;
    m_size = 0; m_mapped = nullptr; m_gpu = nullptr;
}

void Buffer::swap(Buffer& o) noexcept {
    std::swap(m_gpu, o.m_gpu); std::swap(m_buffer, o.m_buffer);
    std::swap(m_alloc, o.m_alloc); std::swap(m_size, o.m_size);
    std::swap(m_mapped, o.m_mapped);
}

// ------------------------------------------------------------------ Image --
void Image::create(GpuContext& gpu, uint32_t w, uint32_t h, VkFormat format,
                   VkImageUsageFlags usage, uint32_t mipLevels) {
    destroy();
    m_gpu = &gpu; m_width = w; m_height = h; m_mips = mipLevels;

    VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = { w, h, 1 };
    ii.mipLevels = mipLevels;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    MRT_VK_CHECK(vmaCreateImage(gpu.allocator(), &ii, &ai, &m_image, &m_alloc, nullptr));

    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = m_image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1 };
    MRT_VK_CHECK(vkCreateImageView(gpu.device(), &vi, nullptr, &m_view));
}

void Image::destroy() {
    if (m_gpu) {
        if (m_view)  vkDestroyImageView(m_gpu->device(), m_view, nullptr);
        if (m_image) vmaDestroyImage(m_gpu->allocator(), m_image, m_alloc);
    }
    m_image = VK_NULL_HANDLE; m_view = VK_NULL_HANDLE; m_alloc = VK_NULL_HANDLE;
    m_width = m_height = 0; m_mips = 1; m_gpu = nullptr;
}

void Image::swap(Image& o) noexcept {
    std::swap(m_gpu, o.m_gpu); std::swap(m_image, o.m_image);
    std::swap(m_view, o.m_view); std::swap(m_alloc, o.m_alloc);
    std::swap(m_width, o.m_width); std::swap(m_height, o.m_height);
    std::swap(m_mips, o.m_mips);
}

// ------------------------------------------------------------- GpuContext --
namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const LogLevel lvl = (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        ? LogLevel::Error
        : (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT ? LogLevel::Warn : LogLevel::Debug);
    log(lvl, std::string("[vk] ") + data->pMessage);
    return VK_FALSE;
}

bool hasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

} // namespace

Result GpuContext::init(const GpuContextDesc& desc) {
    if (Result r = createInstance(desc); r != Result::Success) return r;
    if (Result r = pickDeviceAndQueue(desc.needPresentSupport); r != Result::Success) return r;
    if (Result r = createDevice(desc.needPresentSupport); r != Result::Success) return r;

    VmaAllocatorCreateInfo aci{};
    aci.instance = m_instance;
    aci.physicalDevice = m_physical;
    aci.device = m_device;
    aci.vulkanApiVersion = VK_API_VERSION_1_2;
    MRT_VK_CHECK(vmaCreateAllocator(&aci, &m_allocator));

    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = m_queueFamily;
    MRT_VK_CHECK(vkCreateCommandPool(m_device, &pci, nullptr, &m_cmdPool));

    VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    MRT_VK_CHECK(vkCreateFence(m_device, &fci, nullptr, &m_submitFence));

    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    MRT_VK_CHECK(vkCreateSampler(m_device, &sci, nullptr, &m_linearSampler));

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_physical, &props);
    m_unifiedMemory = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
    log(LogLevel::Info, std::string("GPU: ") + props.deviceName);
    return Result::Success;
}

GpuContext::~GpuContext() {
    if (!m_device) return;
    vkDeviceWaitIdle(m_device);
    m_staging.destroy();
    vkDestroySampler(m_device, m_linearSampler, nullptr);
    vkDestroyFence(m_device, m_submitFence, nullptr);
    vkDestroyCommandPool(m_device, m_cmdPool, nullptr);
    vmaDestroyAllocator(m_allocator);
    vkDestroyDevice(m_device, nullptr);
    if (m_debugMessenger) {
        auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyFn) destroyFn(m_instance, m_debugMessenger, nullptr);
    }
    vkDestroyInstance(m_instance, nullptr);
}

Result GpuContext::createInstance(const GpuContextDesc& desc) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());

    std::vector<const char*> exts = desc.instanceExtensions;
    VkInstanceCreateFlags flags = 0;
    // MoltenVK is a portability (non-conformant) implementation; opting in is
    // mandatory with loaders >= 1.3.216.
    if (hasExtension(available, "VK_KHR_portability_enumeration")) {
        exts.push_back("VK_KHR_portability_enumeration");
        flags |= 0x00000001; // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
    }
    if (hasExtension(available, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
        exts.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    if (desc.enableValidation && hasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    // Only request the validation layer when it is actually installed —
    // bundled (SDK-less) builds would otherwise fail vkCreateInstance.
    std::vector<const char*> layers;
    if (desc.enableValidation) {
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> avail(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, avail.data());
        for (const auto& l : avail) {
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                break;
            }
        }
        if (layers.empty())
            log(LogLevel::Warn, "validation requested but layer not installed; continuing without");
    }

    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "MiniRaytrace";
    app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.flags = flags;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();

    if (vkCreateInstance(&ici, nullptr, &m_instance) != VK_SUCCESS) {
        log(LogLevel::Error, "vkCreateInstance failed (is the Vulkan SDK / MoltenVK installed?)");
        return Result::ErrorVulkanInit;
    }

    if (desc.enableValidation) {
        auto createFn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createFn) {
            VkDebugUtilsMessengerCreateInfoEXT dci{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dci.pfnUserCallback = debugCallback;
            createFn(m_instance, &dci, nullptr, &m_debugMessenger);
        }
    }
    return Result::Success;
}

Result GpuContext::pickDeviceAndQueue(bool /*needPresent*/) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) {
        log(LogLevel::Error, "No Vulkan physical devices found");
        return Result::ErrorVulkanInit;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    // Prefer discrete > integrated > anything; need a GRAPHICS|COMPUTE family
    // (graphics is required for vkCmdBlitImage used in mip generation / present blit).
    int bestScore = -1;
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(d, &props);
        int score = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)   ? 100
                  : (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) ? 50 : 10;

        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qCount, families.data());
        for (uint32_t i = 0; i < qCount; ++i) {
            const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((families[i].queueFlags & need) == need) {
                if (score > bestScore) { bestScore = score; m_physical = d; m_queueFamily = i; }
                break;
            }
        }
    }
    return m_physical ? Result::Success : Result::ErrorVulkanInit;
}

Result GpuContext::createDevice(bool needPresent) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &count, available.data());

    std::vector<const char*> exts;
    // Required by spec whenever the implementation advertises it (MoltenVK does).
    if (hasExtension(available, "VK_KHR_portability_subset"))
        exts.push_back("VK_KHR_portability_subset");
    if (needPresent)
        exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = m_queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    // Materials index the texture array per-pixel (non-dynamically-uniform):
    // request the matching Vulkan 1.2 indexing feature when available.
    // MoltenVK on Apple Silicon exposes it via Metal argument buffers.
    VkPhysicalDeviceVulkan12Features avail12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 avail2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    avail2.pNext = &avail12;
    vkGetPhysicalDeviceFeatures2(m_physical, &avail2);

    VkPhysicalDeviceVulkan12Features enable12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    enable12.shaderSampledImageArrayNonUniformIndexing =
        avail12.shaderSampledImageArrayNonUniformIndexing;
    if (!enable12.shaderSampledImageArrayNonUniformIndexing)
        log(LogLevel::Warn, "shaderSampledImageArrayNonUniformIndexing unsupported; "
                            "per-pixel texture indexing may misbehave");

    VkPhysicalDeviceFeatures2 enable2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    enable2.pNext = &enable12;
    enable2.features.shaderSampledImageArrayDynamicIndexing =
        avail2.features.shaderSampledImageArrayDynamicIndexing;

    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &enable2; // features via pNext chain; pEnabledFeatures stays null
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    dci.ppEnabledExtensionNames = exts.data();

    if (vkCreateDevice(m_physical, &dci, nullptr, &m_device) != VK_SUCCESS)
        return Result::ErrorVulkanInit;
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);
    return Result::Success;
}

void GpuContext::immediateSubmit(const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = m_cmdPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    MRT_VK_CHECK(vkAllocateCommandBuffers(m_device, &cai, &cmd));

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    MRT_VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    record(cmd);
    MRT_VK_CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    MRT_VK_CHECK(vkQueueSubmit(m_queue, 1, &si, m_submitFence));
    MRT_VK_CHECK(vkWaitForFences(m_device, 1, &m_submitFence, VK_TRUE, UINT64_MAX));
    MRT_VK_CHECK(vkResetFences(m_device, 1, &m_submitFence));
    vkFreeCommandBuffers(m_device, m_cmdPool, 1, &cmd);
}

namespace {
// Grow-only staging buffer helper.
void ensureStaging(GpuContext& gpu, Buffer& staging, VkDeviceSize bytes) {
    if (staging.size() < bytes) {
        staging.create(gpu, std::max<VkDeviceSize>(bytes, 4u << 20),
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       /*hostVisible*/ true);
    }
}
} // namespace

void GpuContext::uploadToBuffer(Buffer& dst, const void* src, VkDeviceSize bytes, VkDeviceSize offset) {
    if (bytes == 0) return;
    ensureStaging(*this, m_staging, bytes);
    std::memcpy(m_staging.mapped(), src, bytes);
    immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy region{ 0, offset, bytes };
        vkCmdCopyBuffer(cmd, m_staging.handle(), dst.handle(), 1, &region);
    });
}

void GpuContext::uploadToImage(Image& dst, const void* pixels, size_t bytes, bool genMips) {
    ensureStaging(*this, m_staging, bytes);
    std::memcpy(m_staging.mapped(), pixels, bytes);

    immediateSubmit([&](VkCommandBuffer cmd) {
        imageBarrier(cmd, dst.handle(),
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     0, VK_ACCESS_TRANSFER_WRITE_BIT);

        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { dst.width(), dst.height(), 1 };
        vkCmdCopyBufferToImage(cmd, m_staging.handle(), dst.handle(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        if (genMips && dst.mips() > 1) {
            int32_t w = static_cast<int32_t>(dst.width());
            int32_t h = static_cast<int32_t>(dst.height());
            for (uint32_t mip = 1; mip < dst.mips(); ++mip) {
                imageBarrier(cmd, dst.handle(),
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                             mip - 1, 1);
                VkImageBlit blit{};
                blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 1 };
                blit.srcOffsets[1] = { w, h, 1 };
                w = std::max(1, w / 2); h = std::max(1, h / 2);
                blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1 };
                blit.dstOffsets[1] = { w, h, 1 };
                vkCmdBlitImage(cmd, dst.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               dst.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &blit, VK_FILTER_LINEAR);
                imageBarrier(cmd, dst.handle(),
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                             mip - 1, 1);
            }
            imageBarrier(cmd, dst.handle(),
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                         dst.mips() - 1, 1);
        } else {
            imageBarrier(cmd, dst.handle(),
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
    });
}

void GpuContext::readbackImage(Image& src, Buffer& dst) {
    immediateSubmit([&](VkCommandBuffer cmd) {
        imageBarrier(cmd, src.handle(),
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { src.width(), src.height(), 1 };
        vkCmdCopyImageToBuffer(cmd, src.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               dst.handle(), 1, &region);
        imageBarrier(cmd, src.handle(),
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    });
}

void GpuContext::readbackBuffer(Buffer& src, void* dst, VkDeviceSize bytes, VkDeviceSize offset) {
    ensureStaging(*this, m_staging, bytes);
    immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy region{ offset, 0, bytes };
        vkCmdCopyBuffer(cmd, src.handle(), m_staging.handle(), 1, &region);
    });
    std::memcpy(dst, m_staging.mapped(), bytes);
}

VkShaderModule GpuContext::createShaderModule(std::span<const uint32_t> words) {
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = words.size() * sizeof(uint32_t);
    ci.pCode = words.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    MRT_VK_CHECK(vkCreateShaderModule(m_device, &ci, nullptr, &mod));
    return mod;
}

void GpuContext::imageBarrier(VkCommandBuffer cmd, VkImage img,
                              VkImageLayout from, VkImageLayout to,
                              VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                              VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                              uint32_t baseMip, uint32_t mipCount) {
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, 1 };
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace mrt
