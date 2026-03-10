#include "pictor/batch/radix_sort.h"
#include "pictor/memory/frame_allocator.h"
#include <cstring>

namespace pictor {

void RadixSort::sort(SortPair* pairs, size_t count, FrameAllocator& allocator) {
    // Full 8-pass radix sort for uint64 keys (§6.2)
    sort_range(pairs, count, allocator, 0, 8);
}

void RadixSort::sort_range(SortPair* pairs, size_t count,
                           FrameAllocator& allocator,
                           uint32_t start_byte, uint32_t end_byte) {
    if (count <= 1) return;

    // Allocate temp buffer from frame allocator (§4.3)
    SortPair* temp = allocator.allocate_array<SortPair>(count);
    if (!temp) return; // out of frame memory

    SortPair* src = pairs;
    SortPair* dst = temp;

    // LSB Radix Sort, 8-bit digits (§6.2)
    for (uint32_t byte_idx = start_byte; byte_idx < end_byte; ++byte_idx) {
        // Counting sort for this byte
        uint32_t histogram[256] = {};

        // Count occurrences
        for (size_t i = 0; i < count; ++i) {
            uint8_t digit = static_cast<uint8_t>(src[i].key >> (byte_idx * 8));
            histogram[digit]++;
        }

        // Compute prefix sums (exclusive scan)
        uint32_t prefix[256];
        prefix[0] = 0;
        for (uint32_t i = 1; i < 256; ++i) {
            prefix[i] = prefix[i - 1] + histogram[i - 1];
        }

        // Scatter (stable: preserves order within same digit)
        for (size_t i = 0; i < count; ++i) {
            uint8_t digit = static_cast<uint8_t>(src[i].key >> (byte_idx * 8));
            dst[prefix[digit]++] = src[i];
        }

        // Swap src and dst for next pass
        SortPair* swap = src;
        src = dst;
        dst = swap;
    }

    // If result ended up in temp buffer, copy back to original
    uint32_t passes = end_byte - start_byte;
    if (passes % 2 != 0) {
        std::memcpy(pairs, temp, count * sizeof(SortPair));
    }
}

uint64_t RadixSort::build_sort_key(uint8_t render_pass_id,
                                   uint8_t transparency,
                                   uint16_t shader_key,
                                   uint16_t material_key,
                                   uint32_t depth) {
    // §6.2 bit layout:
    // 63-60: RenderPass ID (4 bit)
    // 59-56: Transparency (4 bit)
    // 55-40: Shader Key (16 bit)
    // 39-24: Material Key (16 bit)
    // 23-0:  Depth (24 bit)
    uint64_t key = 0;
    key |= (static_cast<uint64_t>(render_pass_id & 0xF)) << 60;
    key |= (static_cast<uint64_t>(transparency & 0xF))   << 56;
    key |= (static_cast<uint64_t>(shader_key))            << 40;
    key |= (static_cast<uint64_t>(material_key))          << 24;
    key |= (static_cast<uint64_t>(depth & 0xFFFFFF));
    return key;
}

} // namespace pictor
