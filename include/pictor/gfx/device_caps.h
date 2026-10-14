#pragma once

#include <cstdint>

namespace pictor::gfx {

/// Coarse capability class of a device (spec/architecture/multi-rhi.md §2).
///   A_Raster  : rasterisation only (WebGL2 class)
///   B_Compute : compute + storage, limited extras (WebGPU class)
///   C_Full    : full desktop / console class
enum class CapabilityTier : uint8_t {
    A_Raster = 0,
    B_Compute,
    C_Full,
};

/// Individually queried optional features. Downgrade declarations
/// (`requires` / `fallback`) refer to these.
enum class Feature : uint8_t {
    Compute = 0,
    StorageBufferVertex,
    StorageBufferFragment,
    StorageTexture,
    IndirectDraw,
    IndirectDrawCount,
    MultiDrawIndirect,
    FirstInstance,
    TimestampQuery,
    TimestampInsidePass,
    OcclusionQuery,
    Bindless,
    Msaa4x,
    Msaa8x,
    ExplicitRenderPassMerge,
    FloatRenderTarget,
    TextureCompressionBc,
    TextureCompressionEtc2,
    TextureCompressionAstc,
    DepthClamp,
    Count_
};

/// Fixed-size feature bit set (trivially copyable, no heap).
struct FeatureSet {
    uint64_t bits = 0;

    static constexpr uint64_t mask(Feature f) noexcept {
        return uint64_t{1} << static_cast<uint32_t>(f);
    }
    constexpr bool has(Feature f) const noexcept { return (bits & mask(f)) != 0; }
    constexpr void set(Feature f, bool on = true) noexcept {
        bits = on ? (bits | mask(f)) : (bits & ~mask(f));
    }
    /// True when every feature in `other` is also present here.
    constexpr bool contains(FeatureSet other) const noexcept {
        return (bits & other.bits) == other.bits;
    }
};
static_assert(static_cast<uint32_t>(Feature::Count_) <= 64, "FeatureSet holds at most 64 features");

/// Numeric limits. Defaults are the portable minimum the multi-RHI design
/// targets (WebGPU's `maxBindGroups = 4` etc.); backends overwrite them with
/// queried values.
struct DeviceLimits {
    uint32_t max_bind_groups                    = 4;
    uint32_t max_color_attachments              = 8;
    uint32_t max_vertex_buffers                 = 8;
    uint32_t max_vertex_attributes              = 16;
    uint32_t max_texture_dimension_2d           = 8192;
    uint32_t max_texture_array_layers           = 256;
    uint32_t max_uniform_buffer_binding_size    = 65536;
    uint32_t max_storage_buffer_binding_size    = 134217728;
    uint32_t max_compute_invocations_per_group  = 256;
    uint32_t max_compute_workgroups_per_dim     = 65535;
};

/// Depth range of normalised device coordinates.
enum class ClipSpaceDepth : uint8_t { ZeroToOne = 0, MinusOneToOne };

/// Where texel (0,0) of a render target lies.
enum class FramebufferOrigin : uint8_t { TopLeft = 0, BottomLeft };

/// What the selected device can do. Read-only for consumers: it is shown and
/// recorded, and Pictor resolves downgrades from it at graph compile time.
struct DeviceCaps {
    CapabilityTier    tier         = CapabilityTier::C_Full;
    FeatureSet        features     = {};
    DeviceLimits      limits       = {};
    ClipSpaceDepth    clip_depth   = ClipSpaceDepth::ZeroToOne;
    FramebufferOrigin origin       = FramebufferOrigin::TopLeft;
};

const char* capability_tier_name(CapabilityTier tier) noexcept;
const char* feature_name(Feature feature) noexcept;

} // namespace pictor::gfx
