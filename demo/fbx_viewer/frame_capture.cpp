#include "frame_capture.h"

#include "vk_buffer_util.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace pictor_fbx_viewer {

namespace {

// Minimal 32-bit BGRA BMP writer (bottom-up rows, BI_BITFIELDS not needed
// because we emit plain BGRX with alpha forced opaque).
bool write_bmp32(const std::string& path, uint32_t w, uint32_t h, const uint8_t* bgra) {
    const uint64_t row_bytes64 = static_cast<uint64_t>(w) * 4;
    const uint64_t pixel_bytes64 = row_bytes64 * h;
    const uint64_t file_size64 = 14 + 40 + pixel_bytes64;
    if (!bgra || row_bytes64 > std::numeric_limits<uint32_t>::max() ||
        pixel_bytes64 > std::numeric_limits<uint32_t>::max() ||
        file_size64 > std::numeric_limits<uint32_t>::max()) return false;

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t row_bytes = static_cast<uint32_t>(row_bytes64);
    const uint32_t pixel_bytes = static_cast<uint32_t>(pixel_bytes64);
    const uint32_t file_size = static_cast<uint32_t>(file_size64);
    bool ok = true;

    auto put16 = [&](uint16_t v) { ok = ok && std::fwrite(&v, 2, 1, f) == 1; };
    auto put32 = [&](uint32_t v) { ok = ok && std::fwrite(&v, 4, 1, f) == 1; };
    auto puti32 = [&](int32_t v) { ok = ok && std::fwrite(&v, 4, 1, f) == 1; };

    // BITMAPFILEHEADER
    put16(0x4D42); put32(file_size); put16(0); put16(0); put32(14 + 40);
    // BITMAPINFOHEADER
    put32(40); puti32(static_cast<int32_t>(w)); puti32(static_cast<int32_t>(h));
    put16(1); put16(32); put32(0); put32(pixel_bytes);
    puti32(2835); puti32(2835); put32(0); put32(0);

    std::vector<uint8_t> row(row_bytes);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = bgra + static_cast<size_t>(h - 1 - y) * row_bytes;
        std::memcpy(row.data(), src, row_bytes);
        for (uint32_t x = 0; x < w; ++x) row[x * 4 + 3] = 255;
        ok = ok && std::fwrite(row.data(), 1, row_bytes, f) == row_bytes;
    }
    return std::fclose(f) == 0 && ok;
}

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
bool FrameCapture::supports_format(VkFormat format) {
    return format == VK_FORMAT_R8G8B8A8_SRGB ||
           format == VK_FORMAT_R8G8B8A8_UNORM ||
           format == VK_FORMAT_B8G8R8A8_SRGB ||
           format == VK_FORMAT_B8G8R8A8_UNORM;
}

bool FrameCapture::record(VkCommandBuffer cmd, VkDevice device, VkPhysicalDevice pd,
                          VkImage swapchain_image, VkExtent2D extent,
                          VkFormat swapchain_format) {
    if (!armed_ || recorded_) return false;
    if (!supports_format(swapchain_format) || extent.width == 0 || extent.height == 0) {
        std::fprintf(stderr, "[capture] unsupported format or empty extent\n");
        armed_ = false;
        return false;
    }
    extent_ = extent;
    const VkDeviceSize size = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!create_buffer(device, pd, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, hv, staging_, staging_mem_)) {
        std::fprintf(stderr, "[capture] staging buffer allocation failed\n");
        armed_ = false;
        return false;
    }

    image_barrier(cmd, swapchain_image,
                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, swapchain_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging_, 1, &region);

    image_barrier(cmd, swapchain_image,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_READ_BIT, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    recorded_ = true;
    return true;
}

bool FrameCapture::finish(VkDevice device, VkQueue queue, VkFormat swapchain_format) {
    if (!recorded_) return false;
    vkQueueWaitIdle(queue);
    if (!supports_format(swapchain_format)) {
        std::fprintf(stderr, "[capture] unsupported swapchain format %d\n",
                     static_cast<int>(swapchain_format));
        destroy(device);
        armed_ = false;
        recorded_ = false;
        return false;
    }

    const size_t size = static_cast<size_t>(extent_.width) * extent_.height * 4;
    std::vector<uint8_t> pixels(size);
    void* p = nullptr;
    if (vkMapMemory(device, staging_mem_, 0, size, 0, &p) != VK_SUCCESS) {
        std::fprintf(stderr, "[capture] failed to map staging memory\n");
        destroy(device);
        armed_ = false;
        recorded_ = false;
        return false;
    }
    std::memcpy(pixels.data(), p, size);
    vkUnmapMemory(device, staging_mem_);

    // BMP wants BGRA; swap channels when the swapchain is RGBA.
    const bool rgba = swapchain_format == VK_FORMAT_R8G8B8A8_SRGB ||
                      swapchain_format == VK_FORMAT_R8G8B8A8_UNORM;
    if (rgba) {
        for (size_t i = 0; i + 3 < size; i += 4) std::swap(pixels[i], pixels[i + 2]);
    }

    const bool ok = write_bmp32(path_, extent_.width, extent_.height, pixels.data());
    std::printf("[capture] %s %s (%ux%u)\n", ok ? "wrote" : "FAILED to write",
                path_.c_str(), extent_.width, extent_.height);
    destroy(device);
    armed_ = false;
    recorded_ = false;
    return ok;
}

void FrameCapture::destroy(VkDevice device) {
    if (staging_)     { vkDestroyBuffer(device, staging_, nullptr);  staging_ = VK_NULL_HANDLE; }
    if (staging_mem_) { vkFreeMemory(device, staging_mem_, nullptr); staging_mem_ = VK_NULL_HANDLE; }
}

} // namespace pictor_fbx_viewer
