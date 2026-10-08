// Swapchain image → host-visible staging buffer copy, shared by the one-shot
// BMP capture (FrameCapture) and the per-frame raw output (`--raw-out`,
// SPEC-PC-FBX-TRACK-PLAYBACK). The staging buffer is created once per
// extent and stays mapped, so a per-frame read-back does not allocate.
// Requires a swapchain created with VK_IMAGE_USAGE_TRANSFER_SRC_BIT.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace pictor_fbx_viewer {

class SwapchainReadback {
public:
    /// Four-byte RGBA/BGRA formats only.
    static bool supports_format(VkFormat format);
    static bool is_rgba(VkFormat format) {
        return format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_R8G8B8A8_UNORM;
    }

    /// (Re)create the staging buffer when `extent` changed. Returns false on
    /// allocation or mapping failure.
    bool ensure(VkDevice device, VkPhysicalDevice pd, VkExtent2D extent);

    /// Record the copy. Call after vkCmdEndRenderPass (image in
    /// PRESENT_SRC) and before vkEndCommandBuffer; the image is returned to
    /// PRESENT_SRC afterwards.
    void record(VkCommandBuffer cmd, VkImage swapchain_image) const;

    /// Tightly packed rows, top row first. Valid once the submit that
    /// recorded the copy has completed.
    const uint8_t* pixels() const { return mapped_; }
    VkExtent2D extent() const { return extent_; }

    void destroy(VkDevice device);

private:
    VkExtent2D     extent_{};
    VkBuffer       staging_     = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem_ = VK_NULL_HANDLE;
    uint8_t*       mapped_      = nullptr;
};

} // namespace pictor_fbx_viewer
