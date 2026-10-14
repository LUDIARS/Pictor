#pragma once

#include <cstdint>

namespace pictor::gfx {

/// Backend-neutral pixel / vertex format. Each backend maps it to its own
/// format enum internally; consumers never see those.
enum class GfxFormat : uint8_t {
    Undefined = 0,

    // 8-bit normalised colour
    R8Unorm,
    Rg8Unorm,
    Rgba8Unorm,
    Rgba8UnormSrgb,
    Bgra8Unorm,
    Bgra8UnormSrgb,

    // float / integer colour (also used for vertex attributes)
    R16Float,
    Rg16Float,
    Rgba16Float,
    R32Float,
    Rg32Float,
    Rgb32Float,
    Rgba32Float,
    R32Uint,
    Rg32Uint,
    Rgba32Uint,
    Rgb10A2Unorm,
    Rg11B10Float,

    // depth / stencil
    D16Unorm,
    D24UnormS8Uint,
    D32Float,
    D32FloatS8Uint,

    // block compressed
    Bc1RgbaUnorm,
    Bc3RgbaUnorm,
    Bc7RgbaUnorm,
    Etc2Rgba8Unorm,
    Astc4x4Unorm,

    Count_
};

enum class IndexFormat : uint8_t { Uint16 = 0, Uint32 };

constexpr bool is_depth_format(GfxFormat f) noexcept {
    return f == GfxFormat::D16Unorm || f == GfxFormat::D24UnormS8Uint ||
           f == GfxFormat::D32Float || f == GfxFormat::D32FloatS8Uint;
}

constexpr bool has_stencil(GfxFormat f) noexcept {
    return f == GfxFormat::D24UnormS8Uint || f == GfxFormat::D32FloatS8Uint;
}

constexpr bool is_block_compressed(GfxFormat f) noexcept {
    return f >= GfxFormat::Bc1RgbaUnorm && f <= GfxFormat::Astc4x4Unorm;
}

/// Bytes per texel / element for uncompressed formats; 0 for Undefined and
/// block-compressed formats.
uint32_t format_bytes_per_texel(GfxFormat f) noexcept;

const char* format_name(GfxFormat f) noexcept;

} // namespace pictor::gfx
