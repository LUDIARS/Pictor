// Small Vulkan helpers shared by the fbx_viewer demo modules
// (main viewer, fur shell pass, frame capture). Header-only on purpose:
// these are demo conveniences, not part of the Pictor library surface.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pictor_fbx_viewer {

inline uint32_t find_memory_type(VkPhysicalDevice pd, uint32_t type_filter,
                                 VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(pd, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    return UINT32_MAX;
}

inline bool create_buffer(VkDevice device, VkPhysicalDevice pd,
                          VkDeviceSize size, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags props,
                          VkBuffer& buf, VkDeviceMemory& mem) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size  = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    buf = VK_NULL_HANDLE;
    mem = VK_NULL_HANDLE;
    if (size == 0 || vkCreateBuffer(device, &info, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buf, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = find_memory_type(pd, req.memoryTypeBits, props);
    if (alloc.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(device, &alloc, nullptr, &mem) != VK_SUCCESS) {
        vkDestroyBuffer(device, buf, nullptr);
        buf = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindBufferMemory(device, buf, mem, 0) != VK_SUCCESS) {
        vkFreeMemory(device, mem, nullptr);
        vkDestroyBuffer(device, buf, nullptr);
        mem = VK_NULL_HANDLE;
        buf = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

inline VkShaderModule load_shader_spv(VkDevice device, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "Failed to open shader: %s\n", path.c_str()); return VK_NULL_HANDLE; }
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return VK_NULL_HANDLE; }
    const long len = std::ftell(f);
    if (len <= 0 || (len % static_cast<long>(sizeof(uint32_t))) != 0 ||
        std::fseek(f, 0, SEEK_SET) != 0) {
        std::fprintf(stderr, "Invalid SPIR-V file: %s\n", path.c_str());
        std::fclose(f);
        return VK_NULL_HANDLE;
    }
    std::vector<uint32_t> code(static_cast<size_t>(len) / sizeof(uint32_t));
    const bool read_ok = std::fread(code.data(), 1, static_cast<size_t>(len), f) ==
                         static_cast<size_t>(len);
    std::fclose(f);
    if (!read_ok) {
        std::fprintf(stderr, "Failed to read shader: %s\n", path.c_str());
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = static_cast<size_t>(len);
    info.pCode = code.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &mod) != VK_SUCCESS) return VK_NULL_HANDLE;
    return mod;
}

} // namespace pictor_fbx_viewer
