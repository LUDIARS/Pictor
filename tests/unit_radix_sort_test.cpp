/// RadixSort unit test: monotonic ordering + stability (§6.2).

#include "pictor/batch/radix_sort.h"
#include "pictor/memory/frame_allocator.h"
#include "test_common.h"

#include <cstdint>
#include <random>
#include <vector>

using namespace pictor;
using namespace pictor_test;

int main() {
    // 1. Random uint64 keys → monotonically non-decreasing after sort.
    {
        constexpr size_t N = 10000;
        std::vector<SortPair> pairs(N);
        std::mt19937_64 rng(0xC0FFEEULL);
        for (size_t i = 0; i < N; ++i) {
            pairs[i].key = rng();
            pairs[i].index = static_cast<uint32_t>(i);
            pairs[i].padding = 0;
        }

        FrameAllocator alloc(N * sizeof(SortPair) * 4);
        RadixSort::sort(pairs.data(), N, alloc);

        bool sorted = true;
        for (size_t i = 1; i < N; ++i) {
            if (pairs[i - 1].key > pairs[i].key) { sorted = false; break; }
        }
        PT_ASSERT(sorted, "monotonic non-decreasing after radix sort");
    }

    // 2. Stability: equal keys preserve original index order.
    {
        constexpr size_t N = 1000;
        std::vector<SortPair> pairs(N);
        for (size_t i = 0; i < N; ++i) {
            pairs[i].key = (i / 100) * 7;     // 10 buckets of ~100 each
            pairs[i].index = static_cast<uint32_t>(i);
            pairs[i].padding = 0;
        }

        FrameAllocator alloc(N * sizeof(SortPair) * 4);
        RadixSort::sort(pairs.data(), N, alloc);

        bool stable = true;
        for (size_t i = 1; i < N; ++i) {
            if (pairs[i - 1].key == pairs[i].key &&
                pairs[i - 1].index > pairs[i].index) {
                stable = false; break;
            }
        }
        PT_ASSERT(stable, "stable for equal keys");
    }

    // 3. Index payload survives intact (no data corruption).
    {
        constexpr size_t N = 256;
        std::vector<SortPair> pairs(N);
        std::vector<uint32_t> seen(N, 0);
        std::mt19937_64 rng(42);
        for (size_t i = 0; i < N; ++i) {
            pairs[i].key = rng() & 0xFFFF;
            pairs[i].index = static_cast<uint32_t>(i);
            pairs[i].padding = 0;
        }

        FrameAllocator alloc(64 * 1024);
        RadixSort::sort(pairs.data(), N, alloc);

        for (size_t i = 0; i < N; ++i) {
            PT_ASSERT(pairs[i].index < N, "index in range");
            seen[pairs[i].index]++;
        }
        bool all_present = true;
        for (uint32_t v : seen) if (v != 1) { all_present = false; break; }
        PT_ASSERT(all_present, "every original index appears exactly once");
    }

    // 4. Empty + single-element edge cases.
    {
        FrameAllocator alloc(4096);
        SortPair single{42, 7, 0};
        RadixSort::sort(&single, 1, alloc);
        PT_ASSERT_OP(single.key, ==, 42u, "single elem unchanged");
        PT_ASSERT_OP(single.index, ==, 7u, "single elem unchanged");

        RadixSort::sort(nullptr, 0, alloc); // must not crash
        PT_ASSERT(true, "empty input no-op");
    }

    // 5. build_sort_key bit layout: render_pass dominates.
    {
        uint64_t a = RadixSort::build_sort_key(0, 0, 0, 0, 0xFFFFFF);
        uint64_t b = RadixSort::build_sort_key(1, 0, 0, 0, 0);
        PT_ASSERT(b > a, "render_pass id dominates depth");
    }

    return report("unit_radix_sort_test");
}
