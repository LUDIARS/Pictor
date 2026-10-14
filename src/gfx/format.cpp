#include "pictor/gfx/format.h"

namespace pictor::gfx {

uint32_t format_bytes_per_texel(GfxFormat f) noexcept {
    switch (f) {
        case GfxFormat::R8Unorm:        return 1;
        case GfxFormat::Rg8Unorm:       return 2;
        case GfxFormat::Rgba8Unorm:
        case GfxFormat::Rgba8UnormSrgb:
        case GfxFormat::Bgra8Unorm:
        case GfxFormat::Bgra8UnormSrgb: return 4;
        case GfxFormat::R16Float:       return 2;
        case GfxFormat::Rg16Float:      return 4;
        case GfxFormat::Rgba16Float:    return 8;
        case GfxFormat::R32Float:       return 4;
        case GfxFormat::Rg32Float:      return 8;
        case GfxFormat::Rgb32Float:     return 12;
        case GfxFormat::Rgba32Float:    return 16;
        case GfxFormat::R32Uint:        return 4;
        case GfxFormat::Rg32Uint:       return 8;
        case GfxFormat::Rgba32Uint:     return 16;
        case GfxFormat::Rgb10A2Unorm:
        case GfxFormat::Rg11B10Float:   return 4;
        case GfxFormat::D16Unorm:       return 2;
        case GfxFormat::D24UnormS8Uint:
        case GfxFormat::D32Float:       return 4;
        case GfxFormat::D32FloatS8Uint: return 8;
        case GfxFormat::Undefined:
        case GfxFormat::Bc1RgbaUnorm:
        case GfxFormat::Bc3RgbaUnorm:
        case GfxFormat::Bc7RgbaUnorm:
        case GfxFormat::Etc2Rgba8Unorm:
        case GfxFormat::Astc4x4Unorm:
        case GfxFormat::Count_:         break;
    }
    return 0;
}

const char* format_name(GfxFormat f) noexcept {
    switch (f) {
        case GfxFormat::Undefined:      return "Undefined";
        case GfxFormat::R8Unorm:        return "R8Unorm";
        case GfxFormat::Rg8Unorm:       return "Rg8Unorm";
        case GfxFormat::Rgba8Unorm:     return "Rgba8Unorm";
        case GfxFormat::Rgba8UnormSrgb: return "Rgba8UnormSrgb";
        case GfxFormat::Bgra8Unorm:     return "Bgra8Unorm";
        case GfxFormat::Bgra8UnormSrgb: return "Bgra8UnormSrgb";
        case GfxFormat::R16Float:       return "R16Float";
        case GfxFormat::Rg16Float:      return "Rg16Float";
        case GfxFormat::Rgba16Float:    return "Rgba16Float";
        case GfxFormat::R32Float:       return "R32Float";
        case GfxFormat::Rg32Float:      return "Rg32Float";
        case GfxFormat::Rgb32Float:     return "Rgb32Float";
        case GfxFormat::Rgba32Float:    return "Rgba32Float";
        case GfxFormat::R32Uint:        return "R32Uint";
        case GfxFormat::Rg32Uint:       return "Rg32Uint";
        case GfxFormat::Rgba32Uint:     return "Rgba32Uint";
        case GfxFormat::Rgb10A2Unorm:   return "Rgb10A2Unorm";
        case GfxFormat::Rg11B10Float:   return "Rg11B10Float";
        case GfxFormat::D16Unorm:       return "D16Unorm";
        case GfxFormat::D24UnormS8Uint: return "D24UnormS8Uint";
        case GfxFormat::D32Float:       return "D32Float";
        case GfxFormat::D32FloatS8Uint: return "D32FloatS8Uint";
        case GfxFormat::Bc1RgbaUnorm:   return "Bc1RgbaUnorm";
        case GfxFormat::Bc3RgbaUnorm:   return "Bc3RgbaUnorm";
        case GfxFormat::Bc7RgbaUnorm:   return "Bc7RgbaUnorm";
        case GfxFormat::Etc2Rgba8Unorm: return "Etc2Rgba8Unorm";
        case GfxFormat::Astc4x4Unorm:   return "Astc4x4Unorm";
        case GfxFormat::Count_:         break;
    }
    return "unknown";
}

} // namespace pictor::gfx
