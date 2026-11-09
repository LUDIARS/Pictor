#include "host_buffer.h"

#ifdef PICTOR_HAS_VULKAN

#include <cstdint>

namespace pictor::vector_text_detail {

namespace {
uint32_t find_memory_type(VkPhysicalDevice physical, uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((filter & (1U << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    return UINT32_MAX;
}
}  // namespace

HostBuffer::~HostBuffer() { release(); }

void HostBuffer::release() {
    if (mapped_ != nullptr) vkUnmapMemory(device_, memory_);
    if (buffer_ != VK_NULL_HANDLE) vkDestroyBuffer(device_, buffer_, nullptr);
    if (memory_ != VK_NULL_HANDLE) vkFreeMemory(device_, memory_, nullptr);
    mapped_   = nullptr;
    buffer_   = VK_NULL_HANDLE;
    memory_   = VK_NULL_HANDLE;
    capacity_ = 0;
}

bool HostBuffer::allocate(size_t bytes) {
    release();
    if (bytes == 0) bytes = 256;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size        = bytes;
    bi.usage       = usage_;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bi, nullptr, &buffer_) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, buffer_, &req);
    const uint32_t type = find_memory_type(physical_, req.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        release();
        return false;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(device_, &ai, nullptr, &memory_) != VK_SUCCESS ||
        vkBindBufferMemory(device_, buffer_, memory_, 0) != VK_SUCCESS ||
        vkMapMemory(device_, memory_, 0, VK_WHOLE_SIZE, 0, &mapped_) != VK_SUCCESS) {
        release();
        return false;
    }
    capacity_ = bytes;
    return true;
}

}  // namespace pictor::vector_text_detail

#endif  // PICTOR_HAS_VULKAN
