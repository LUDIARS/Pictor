#include "rhi/common/handle_allocator.h"

namespace pictor::rhi {

uint32_t HandleAllocator::allocate() {
    uint32_t index;
    if (!free_slots_.empty()) {
        index = free_slots_.back();
        free_slots_.pop_back();
    } else {
        if (generations_.size() >= kMaxSlots) return 0;
        index = static_cast<uint32_t>(generations_.size());
        generations_.push_back(1);
        live_.push_back(0);
    }
    live_[index] = 1;
    ++live_count_;
    return (uint32_t{generations_[index]} << kIndexBits) | index;
}

bool HandleAllocator::release(uint32_t bits) {
    if (!is_alive(bits)) return false;
    const uint32_t index = bits & kIndexMask;
    live_[index] = 0;
    uint32_t next = uint32_t{generations_[index]} + 1;
    if (next > kMaxGeneration) next = 1;  // generation 0 is reserved for "invalid"
    generations_[index] = static_cast<uint16_t>(next);
    free_slots_.push_back(index);
    --live_count_;
    return true;
}

} // namespace pictor::rhi
