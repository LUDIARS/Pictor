#include "vr_vulkan_util.h"

#include <cstdio>
#include <fstream>
#include <vector>

namespace vr_demo {

namespace {

bool find_memory_type(VkPhysicalDevice physical, uint32_t type_bits,
                      VkMemoryPropertyFlags wanted, uint32_t& out) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & wanted) == wanted) {
            out = i;
            return true;
        }
    }
    return false;
}

} // namespace

bool create_image(VkPhysicalDevice physical, VkDevice device, const ImageDesc& desc, Image& out) {
    VkImageCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.format        = desc.format;
    info.extent        = {desc.width, desc.height, 1};
    info.mipLevels     = 1;
    info.arrayLayers   = desc.layers;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.usage         = desc.usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &info, nullptr, &out.image) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, out.image, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    if (!find_memory_type(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          alloc.memoryTypeIndex) ||
        vkAllocateMemory(device, &alloc, nullptr, &out.memory) != VK_SUCCESS ||
        vkBindImageMemory(device, out.image, out.memory, 0) != VK_SUCCESS) {
        destroy_image(device, out);
        return false;
    }

    VkImageViewCreateInfo view{};
    view.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image    = out.image;
    view.viewType = desc.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    view.format   = desc.format;
    view.subresourceRange.aspectMask = desc.aspect;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = desc.layers;
    if (vkCreateImageView(device, &view, nullptr, &out.view) != VK_SUCCESS) {
        destroy_image(device, out);
        return false;
    }
    return true;
}

void destroy_image(VkDevice device, Image& image) {
    if (image.view)   vkDestroyImageView(device, image.view, nullptr);
    if (image.image)  vkDestroyImage(device, image.image, nullptr);
    if (image.memory) vkFreeMemory(device, image.memory, nullptr);
    image = Image{};
}

bool create_host_buffer(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size,
                        VkBufferUsageFlags usage, Buffer& out) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size  = size;
    info.usage = usage;
    if (vkCreateBuffer(device, &info, nullptr, &out.buffer) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, out.buffer, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    const VkMemoryPropertyFlags wanted =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!find_memory_type(physical, req.memoryTypeBits, wanted, alloc.memoryTypeIndex) ||
        vkAllocateMemory(device, &alloc, nullptr, &out.memory) != VK_SUCCESS ||
        vkBindBufferMemory(device, out.buffer, out.memory, 0) != VK_SUCCESS ||
        vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) != VK_SUCCESS) {
        destroy_buffer(device, out);
        return false;
    }
    return true;
}

void destroy_buffer(VkDevice device, Buffer& buffer) {
    // マップはメモリの解放で暗黙に外れる。
    if (buffer.buffer) vkDestroyBuffer(device, buffer.buffer, nullptr);
    if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
    buffer = Buffer{};
}

VkShaderModule load_shader_module(VkDevice device, const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::fprintf(stderr, "[vr_demo] shader not found: %s\n", path.c_str());
        return VK_NULL_HANDLE;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0 || (size % 4) != 0) {
        std::fprintf(stderr, "[vr_demo] not a SPIR-V file: %s\n", path.c_str());
        return VK_NULL_HANDLE;
    }
    std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), size);

    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = static_cast<size_t>(size);
    info.pCode    = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
    return module;
}

} // namespace vr_demo
