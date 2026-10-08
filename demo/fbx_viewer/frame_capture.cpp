#include "frame_capture.h"

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

} // namespace

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
bool FrameCapture::supports_format(VkFormat format) {
    return SwapchainReadback::supports_format(format);
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
    if (!readback_.ensure(device, pd, extent)) {
        std::fprintf(stderr, "[capture] staging buffer allocation failed\n");
        armed_ = false;
        return false;
    }
    readback_.record(cmd, swapchain_image);
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

    const VkExtent2D extent = readback_.extent();
    const size_t size = static_cast<size_t>(extent.width) * extent.height * 4;
    std::vector<uint8_t> pixels(readback_.pixels(), readback_.pixels() + size);

    // BMP wants BGRA; swap channels when the swapchain is RGBA.
    if (SwapchainReadback::is_rgba(swapchain_format)) {
        for (size_t i = 0; i + 3 < size; i += 4) std::swap(pixels[i], pixels[i + 2]);
    }

    const bool ok = write_bmp32(path_, extent.width, extent.height, pixels.data());
    std::printf("[capture] %s %s (%ux%u)\n", ok ? "wrote" : "FAILED to write",
                path_.c_str(), extent.width, extent.height);
    destroy(device);
    armed_ = false;
    recorded_ = false;
    return ok;
}

void FrameCapture::destroy(VkDevice device) {
    readback_.destroy(device);
}

} // namespace pictor_fbx_viewer
