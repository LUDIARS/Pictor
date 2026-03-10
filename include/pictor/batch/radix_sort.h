#pragma once

#include <cstdint>
#include <cstddef>

namespace pictor {

class FrameAllocator;

/// Key-index pair for indirect sort (§6.2).
/// Only this pair is sorted; actual data stays in place and is accessed via indirection.
struct SortPair {
    uint64_t key;
    uint32_t index;
    uint32_t padding; // align to 16 bytes
};

static_assert(sizeof(SortPair) == 16, "SortPair must be 16 bytes");

/// Radix Sort implementation for sort keys (§6.2).
/// LSB-first, 8-bit digit, 8 passes for uint64 keys.
/// Stable sort. O(n) time complexity.
class RadixSort {
public:
    /// Sort `count` key-index pairs in-place.
    /// Uses frame allocator for temporary buffer.
    /// @param pairs     Array of SortPair to sort
    /// @param count     Number of elements
    /// @param allocator Frame allocator for temp buffer
    static void sort(SortPair* pairs, size_t count, FrameAllocator& allocator);

    /// Sort only by specific byte range (partial sort for specific bit fields)
    /// @param start_byte First byte to sort by (0 = LSB)
    /// @param end_byte   Last byte (exclusive)
    static void sort_range(SortPair* pairs, size_t count,
                           FrameAllocator& allocator,
                           uint32_t start_byte, uint32_t end_byte);

    /// Build sort key from components (§6.2 bit layout)
    static uint64_t build_sort_key(uint8_t  render_pass_id,   // bits 63-60
                                   uint8_t  transparency,      // bits 59-56
                                   uint16_t shader_key,        // bits 55-40
                                   uint16_t material_key,      // bits 39-24
                                   uint32_t depth);            // bits 23-0 (24-bit)
};

} // namespace pictor
