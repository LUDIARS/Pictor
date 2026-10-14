#pragma once

#include <cstdint>

namespace pictor::gfx {

/// Generation-tagged 32-bit resource handle.
///
///   bits = (generation << kIndexBits) | index
///
/// Generation 0 is never issued, so `bits == 0` (the default) is the invalid
/// handle. A slot's generation advances on every release, which makes a
/// handle to a destroyed resource detectable ("stale") instead of silently
/// aliasing the slot's next occupant. The Tag parameter only keeps buffer /
/// texture / ... handles from converting into each other.
template <typename Tag>
struct Handle {
    static constexpr uint32_t kIndexBits      = 20;
    static constexpr uint32_t kGenerationBits = 32 - kIndexBits;
    static constexpr uint32_t kIndexMask      = (uint32_t{1} << kIndexBits) - 1;
    static constexpr uint32_t kMaxIndex       = kIndexMask;
    static constexpr uint32_t kMaxGeneration  = (uint32_t{1} << kGenerationBits) - 1;

    uint32_t bits = 0;

    static constexpr Handle from_bits(uint32_t raw) noexcept { return Handle{raw}; }
    static constexpr Handle make(uint32_t index, uint32_t generation) noexcept {
        return Handle{((generation & kMaxGeneration) << kIndexBits) | (index & kIndexMask)};
    }

    constexpr uint32_t index() const noexcept { return bits & kIndexMask; }
    constexpr uint32_t generation() const noexcept { return bits >> kIndexBits; }
    constexpr bool     valid() const noexcept { return generation() != 0; }

    friend constexpr bool operator==(Handle a, Handle b) noexcept { return a.bits == b.bits; }
    friend constexpr bool operator!=(Handle a, Handle b) noexcept { return a.bits != b.bits; }
};

struct BufferTag {};
struct TextureTag {};
struct SamplerTag {};
struct PipelineTag {};
struct BindGroupTag {};
struct RenderTargetSetTag {};

using BufferId          = Handle<BufferTag>;
using TextureId         = Handle<TextureTag>;
using SamplerId         = Handle<SamplerTag>;
using PipelineId        = Handle<PipelineTag>;
using BindGroupId       = Handle<BindGroupTag>;
using RenderTargetSetId = Handle<RenderTargetSetTag>;

static_assert(sizeof(BufferId) == 4, "handles are 32-bit");

} // namespace pictor::gfx
