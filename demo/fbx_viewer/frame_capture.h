// One-shot swapchain read-back for the FBX viewer (`--capture <file.bmp>`).
//
// Lets the demo be verified without a human at the screen: after the
// requested frame is recorded, the presented image is copied into a
// host-visible buffer and written as a 32-bit BMP. Requires the swapchain
// to be created with VK_IMAGE_USAGE_TRANSFER_SRC_BIT (VulkanContext does).
#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <utility>

namespace pictor_fbx_viewer {

class FrameCapture {
public:
    /// Arm a capture. `frame_index` is the 0-based frame number at which
    /// the copy is recorded; the file is written once that submit has
    /// completed.
    void request(std::string path, uint32_t frame_index) {
        path_ = std::move(path);
        target_frame_ = frame_index;
        armed_ = true;
    }
    bool armed() const { return armed_; }
    bool should_capture(uint32_t frame_index) const { return armed_ && frame_index >= target_frame_; }

    /// The BMP readback path currently supports only four-byte RGBA/BGRA
    /// swapchain formats.
    static bool supports_format(VkFormat format);

    /// Record the copy commands. Must be called after vkCmdEndRenderPass
    /// (the image is in PRESENT_SRC layout) and before vkEndCommandBuffer;
    /// the image is returned to PRESENT_SRC afterwards.
    bool record(VkCommandBuffer cmd, VkDevice device, VkPhysicalDevice pd,
                VkImage swapchain_image, VkExtent2D extent, VkFormat swapchain_format);

    /// Wait for the GPU, write the BMP, release the staging buffer.
    /// Returns true when the file was written.
    bool finish(VkDevice device, VkQueue queue, VkFormat swapchain_format);

    void destroy(VkDevice device);

private:
    std::string    path_;
    uint32_t       target_frame_ = 0;
    bool           armed_    = false;
    bool           recorded_ = false;
    VkExtent2D     extent_{};
    VkBuffer       staging_     = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem_ = VK_NULL_HANDLE;
};

} // namespace pictor_fbx_viewer
