#pragma once

#include "pictor/gfx/handle.h"

#include <cstdint>
#include <vector>

namespace pictor::rhi {

/// Slot allocator behind every generation-tagged gfx handle.
///
/// Flat arrays only: one generation per slot plus a LIFO free list. allocate /
/// release run at resource creation and destruction (never per draw); lookups
/// on the hot path are `is_alive(bits)`, a bounds check and one compare.
///
/// Encoding matches pictor::gfx::Handle (20-bit index, 12-bit generation,
/// generation 0 never issued). A slot's generation advances on release and
/// wraps from the maximum back to 1, so stale handles stop resolving.
class HandleAllocator {
public:
    static constexpr uint32_t kIndexBits      = 20;
    static constexpr uint32_t kIndexMask      = (uint32_t{1} << kIndexBits) - 1;
    static constexpr uint32_t kMaxSlots       = kIndexMask + 1;
    static constexpr uint32_t kMaxGeneration  = (uint32_t{1} << (32 - kIndexBits)) - 1;

    /// Returns raw handle bits, or 0 when all kMaxSlots slots are live.
    uint32_t allocate();

    /// Frees the slot named by `bits`. Returns false (and changes nothing)
    /// for the invalid handle, a never-issued index or a stale generation.
    bool release(uint32_t bits);

    bool is_alive(uint32_t bits) const noexcept {
        const uint32_t index = bits & kIndexMask;
        const uint32_t gen   = bits >> kIndexBits;
        return gen != 0 && index < generations_.size() &&
               live_[index] != 0 && generations_[index] == gen;
    }

    uint32_t live_count() const noexcept { return live_count_; }
    uint32_t slot_count() const noexcept { return static_cast<uint32_t>(generations_.size()); }

private:
    static_assert(kIndexBits == gfx::BufferId::kIndexBits,
                  "allocator and gfx::Handle must share one bit layout");
    static_assert(kMaxGeneration == gfx::BufferId::kMaxGeneration,
                  "allocator and gfx::Handle must share one bit layout");

    std::vector<uint16_t> generations_;  ///< current generation of each slot
    std::vector<uint8_t>  live_;         ///< 1 while the slot is allocated
    std::vector<uint32_t> free_slots_;   ///< released slot indices (LIFO)
    uint32_t              live_count_ = 0;
};

} // namespace pictor::rhi
