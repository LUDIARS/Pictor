#pragma once

#include "pictor/gfx/compare_op.h"
#include "pictor/gfx/format.h"

#include <cstdint>

namespace pictor::gfx {

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------

/// Bit mask of how a buffer is used (combine with `|`).
enum BufferUsage : uint32_t {
    BufferUsageVertex   = 1u << 0,
    BufferUsageIndex    = 1u << 1,
    BufferUsageUniform  = 1u << 2,
    BufferUsageStorage  = 1u << 3,
    BufferUsageIndirect = 1u << 4,
    BufferUsageCopySrc  = 1u << 5,
    BufferUsageCopyDst  = 1u << 6,
};

/// Who writes / reads the memory. The backend picks heap / staging from it.
enum class MemoryAccess : uint8_t {
    GpuOnly = 0,   ///< written by uploads or the GPU, never mapped
    Upload,        ///< CPU writes every frame (ring / UBO)
    Readback,      ///< GPU writes, CPU reads back
};

struct BufferDesc {
    uint64_t     size       = 0;
    uint32_t     usage      = 0;              ///< BufferUsage bits
    MemoryAccess memory     = MemoryAccess::GpuOnly;
    const char*  debug_name = nullptr;        ///< optional, not copied
};

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

enum class TextureDimension : uint8_t {
    Tex2D = 0,
    Tex2DArray,
    Cube,
    Tex3D,
};

enum TextureUsage : uint32_t {
    TextureUsageSampled     = 1u << 0,
    TextureUsageStorage     = 1u << 1,
    TextureUsageColorTarget = 1u << 2,
    TextureUsageDepthTarget = 1u << 3,
    TextureUsageCopySrc     = 1u << 4,
    TextureUsageCopyDst     = 1u << 5,
};

struct TextureDesc {
    TextureDimension dimension       = TextureDimension::Tex2D;
    GfxFormat        format          = GfxFormat::Rgba8Unorm;
    uint32_t         width           = 1;
    uint32_t         height          = 1;
    uint32_t         depth_or_layers = 1;
    uint32_t         mip_levels      = 1;
    uint32_t         sample_count    = 1;
    uint32_t         usage           = TextureUsageSampled;  ///< TextureUsage bits
    const char*      debug_name      = nullptr;              ///< optional, not copied
};

// ---------------------------------------------------------------------------
// Samplers
// ---------------------------------------------------------------------------

enum class FilterMode : uint8_t { Nearest = 0, Linear };

enum class AddressMode : uint8_t {
    Repeat = 0,
    MirrorRepeat,
    ClampToEdge,
};

struct SamplerDesc {
    FilterMode  min_filter     = FilterMode::Linear;
    FilterMode  mag_filter     = FilterMode::Linear;
    FilterMode  mip_filter     = FilterMode::Linear;
    AddressMode address_u      = AddressMode::Repeat;
    AddressMode address_v      = AddressMode::Repeat;
    AddressMode address_w      = AddressMode::Repeat;
    bool        compare_enable = false;          ///< shadow sampler
    CompareOp   compare        = CompareOp::LessOrEqual;
    uint8_t     max_anisotropy = 1;
    float       lod_min        = 0.0f;
    float       lod_max        = 1000.0f;
};

} // namespace pictor::gfx
