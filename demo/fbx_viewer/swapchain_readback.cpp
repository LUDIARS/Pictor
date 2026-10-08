#include "swapchain_readback.h"

#include "vk_buffer_util.h"

namespace pictor_fbx_viewer {

namespace {

void image_barrier(VkCommandBuffer cmd, VkImage image,
                   VkImageLayout from, VkImageLayout to,
                   VkAccessFlags src_access, VkAccessFlags dst_access,
                   VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
bool SwapchainReadback::supports_format(VkFormat format) {
    return format == VK_FORMAT_R8G8B8A8_SRGB ||
           format == VK_FORMAT_R8G8B8A8_UNORM ||
           format == VK_FORMAT_B8G8R8A8_SRGB ||
           format == VK_FORMAT_B8G8R8A8_UNORM;
}

/// @implements SPEC-PC-FBX-TRACK-PLAYBACK
bool SwapchainReadback::ensure(VkDevice device, VkPhysicalDevice pd, VkExtent2D extent) {
    if (staging_ && extent.width == extent_.width && extent.height == extent_.height) return true;
    destroy(device);
    if (extent.width == 0 || extent.height == 0) return false;
    const VkDeviceSize size = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!create_buffer(device, pd, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, hv, staging_, staging_mem_)) return false;
    void* p = nullptr;
    if (vkMapMemory(device, staging_mem_, 0, size, 0, &p) != VK_SUCCESS) {
        destroy(device);
        return false;
    }
    mapped_ = static_cast<uint8_t*>(p);
    extent_ = extent;
    return true;
}

void SwapchainReadback::record(VkCommandBuffer cmd, VkImage swapchain_image) const {
    image_barrier(cmd, swapchain_image,
                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {extent_.width, extent_.height, 1};
    vkCmdCopyImageToBuffer(cmd, swapchain_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging_, 1, &region);

    image_barrier(cmd, swapchain_image,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_READ_BIT, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

void SwapchainReadback::destroy(VkDevice device) {
    if (mapped_)      { vkUnmapMemory(device, staging_mem_); mapped_ = nullptr; }
    if (staging_)     { vkDestroyBuffer(device, staging_, nullptr);  staging_ = VK_NULL_HANDLE; }
    if (staging_mem_) { vkFreeMemory(device, staging_mem_, nullptr); staging_mem_ = VK_NULL_HANDLE; }
    extent_ = {};
}

} // namespace pictor_fbx_viewer
