#pragma once
// Vulkan device context + RAII resource wrappers (internal header).
// Targets Vulkan 1.2 core; on macOS runs through MoltenVK and therefore
// enables VK_KHR_portability_enumeration / VK_KHR_portability_subset.

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "mrt/Common.hpp"

#include <cstdlib>
#include <functional>
#include <span>
#include <vector>

namespace mrt {

#define MRT_VK_CHECK(expr)                                                     \
    do {                                                                       \
        VkResult _r = (expr);                                                  \
        if (_r != VK_SUCCESS) {                                                \
            mrt::log(mrt::LogLevel::Error,                                     \
                     std::string("Vulkan error ") + std::to_string(_r) +       \
                     " at " #expr);                                            \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

class GpuContext;

// Device-local (optionally host-visible on UMA) buffer.
class Buffer {
public:
    Buffer() = default;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& o) noexcept { swap(o); }
    Buffer& operator=(Buffer&& o) noexcept { destroy(); swap(o); return *this; }
    ~Buffer() { destroy(); }

    void create(GpuContext& gpu, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible);
    void destroy();
    void swap(Buffer& o) noexcept;

    VkBuffer     handle() const { return m_buffer; }
    VkDeviceSize size()   const { return m_size; }
    void*        mapped() const { return m_mapped; } // non-null iff hostVisible
    bool         valid()  const { return m_buffer != VK_NULL_HANDLE; }

private:
    GpuContext*   m_gpu = nullptr;
    VkBuffer      m_buffer = VK_NULL_HANDLE;
    VmaAllocation m_alloc  = VK_NULL_HANDLE;
    VkDeviceSize  m_size   = 0;
    void*         m_mapped = nullptr;
};

// 2D image with a single view; storage and/or sampled usage.
class Image {
public:
    Image() = default;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    Image(Image&& o) noexcept { swap(o); }
    Image& operator=(Image&& o) noexcept { destroy(); swap(o); return *this; }
    ~Image() { destroy(); }

    void create(GpuContext& gpu, uint32_t w, uint32_t h, VkFormat format,
                VkImageUsageFlags usage, uint32_t mipLevels = 1);
    void destroy();
    void swap(Image& o) noexcept;

    VkImage     handle() const { return m_image; }
    VkImageView view()   const { return m_view; }
    uint32_t    width()  const { return m_width; }
    uint32_t    height() const { return m_height; }
    uint32_t    mips()   const { return m_mips; }
    bool        valid()  const { return m_image != VK_NULL_HANDLE; }

private:
    GpuContext*   m_gpu = nullptr;
    VkImage       m_image = VK_NULL_HANDLE;
    VkImageView   m_view  = VK_NULL_HANDLE;
    VmaAllocation m_alloc = VK_NULL_HANDLE;
    uint32_t      m_width = 0, m_height = 0, m_mips = 1;
};

struct GpuContextDesc {
    bool enableValidation   = false;
    bool needPresentSupport = false;
    std::vector<const char*> instanceExtensions;
};

class GpuContext {
public:
    Result init(const GpuContextDesc& desc);
    ~GpuContext();

    VkInstance       instance()       const { return m_instance; }
    VkPhysicalDevice physicalDevice() const { return m_physical; }
    VkDevice         device()         const { return m_device; }
    VkQueue          queue()          const { return m_queue; }
    uint32_t         queueFamily()    const { return m_queueFamily; }
    VmaAllocator     allocator()      const { return m_allocator; }
    VkSampler        linearSampler()  const { return m_linearSampler; }
    bool             unifiedMemory()  const { return m_unifiedMemory; }

    // Record + submit + wait. Used for uploads and (v1) per-frame rendering.
    void immediateSubmit(const std::function<void(VkCommandBuffer)>& record);

    // Staged upload into a device buffer at byte offset.
    void uploadToBuffer(Buffer& dst, const void* src, VkDeviceSize bytes, VkDeviceSize offset = 0);
    // Upload pixels into mip 0 and optionally generate the remaining mips.
    // Leaves the image in SHADER_READ_ONLY_OPTIMAL.
    void uploadToImage(Image& dst, const void* pixels, size_t bytes, bool genMips);
    // Copy a GENERAL-layout image into a host-visible buffer (blocking).
    void readbackImage(Image& src, Buffer& dst);
    void readbackBuffer(Buffer& src, void* dst, VkDeviceSize bytes, VkDeviceSize offset = 0);

    VkShaderModule createShaderModule(std::span<const uint32_t> words);

    static void imageBarrier(VkCommandBuffer cmd, VkImage img,
                             VkImageLayout from, VkImageLayout to,
                             VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                             VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                             uint32_t baseMip = 0, uint32_t mipCount = VK_REMAINING_MIP_LEVELS);

private:
    Result createInstance(const GpuContextDesc& desc);
    Result pickDeviceAndQueue(bool needPresent);
    Result createDevice(bool needPresent);

    VkInstance       m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice         m_device   = VK_NULL_HANDLE;
    VkQueue          m_queue    = VK_NULL_HANDLE;
    uint32_t         m_queueFamily = 0;
    VmaAllocator     m_allocator = VK_NULL_HANDLE;
    VkCommandPool    m_cmdPool   = VK_NULL_HANDLE;
    VkFence          m_submitFence = VK_NULL_HANDLE;
    VkSampler        m_linearSampler = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    bool             m_unifiedMemory = false;
    Buffer           m_staging; // reusable staging buffer, grown on demand
};

} // namespace mrt
