#pragma once

#ifndef NOGDI
#define NOGDI
#endif
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

namespace vr_demo {

/// VkImage + メモリ + view の組。 所有者が destroy_image() で解放する。
struct Image {
    VkImage        image  = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView    view   = VK_NULL_HANDLE;
};

struct Buffer {
    VkBuffer       buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void*          mapped = nullptr;  // host-visible のとき常時マップ
};

struct ImageDesc {
    uint32_t           width  = 0;
    uint32_t           height = 0;
    uint32_t           layers = 1;
    VkFormat           format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags  usage  = 0;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};

bool create_image(VkPhysicalDevice physical, VkDevice device, const ImageDesc& desc, Image& out);
void destroy_image(VkDevice device, Image& image);

/// host から書ける (常時マップ済みの) バッファ。
bool create_host_buffer(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size,
                        VkBufferUsageFlags usage, Buffer& out);
void destroy_buffer(VkDevice device, Buffer& buffer);

/// 失敗時は VK_NULL_HANDLE (理由は stderr)。
VkShaderModule load_shader_module(VkDevice device, const std::string& path);

} // namespace vr_demo
