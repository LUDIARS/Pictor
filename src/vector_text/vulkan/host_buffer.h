#pragma once
// Persistently mapped, host-coherent Vulkan buffer (internal to vector_text).
#ifdef PICTOR_HAS_VULKAN

#include <cstddef>
#include <vulkan/vulkan.h>

namespace pictor::vector_text_detail {

class HostBuffer {
public:
    HostBuffer(VkPhysicalDevice physical, VkDevice device, VkBufferUsageFlags usage)
        : physical_(physical), device_(device), usage_(usage) {}
    ~HostBuffer();
    HostBuffer(const HostBuffer&) = delete;
    HostBuffer& operator=(const HostBuffer&) = delete;

    /// (Re)creates the buffer with at least `bytes` of room; old content is lost.
    bool allocate(size_t bytes);
    void release();

    [[nodiscard]] VkBuffer buffer() const { return buffer_; }
    [[nodiscard]] size_t capacity() const { return capacity_; }
    [[nodiscard]] void* mapped() const { return mapped_; }

private:
    VkPhysicalDevice   physical_;
    VkDevice           device_;
    VkBufferUsageFlags usage_;
    VkBuffer           buffer_   = VK_NULL_HANDLE;
    VkDeviceMemory     memory_   = VK_NULL_HANDLE;
    void*              mapped_   = nullptr;
    size_t             capacity_ = 0;
};

}  // namespace pictor::vector_text_detail

#endif  // PICTOR_HAS_VULKAN
