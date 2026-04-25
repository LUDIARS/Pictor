/// FrameAllocator unit test: bump allocation / alignment / reset (§4.3).

#include "pictor/memory/frame_allocator.h"
#include "test_common.h"

#include <cstdint>

using namespace pictor;
using namespace pictor_test;

int main() {
    // 1. Basic allocate + monotonic bump.
    {
        FrameAllocator a(4096);
        PT_ASSERT_OP(a.capacity(), ==, 4096u, "capacity");
        PT_ASSERT_OP(a.used(), ==, 0u, "initial used");

        void* p1 = a.allocate(64, 16);
        PT_ASSERT(p1 != nullptr, "first allocation");
        PT_ASSERT_OP(reinterpret_cast<uintptr_t>(p1) % 16, ==, 0u, "16B align");

        void* p2 = a.allocate(64, 16);
        PT_ASSERT(p2 != nullptr, "second allocation");
        PT_ASSERT(p2 > p1, "bump pointer monotonic");
        PT_ASSERT_OP(a.used(), >=, 128u, "used >= 128");
    }

    // 2. Alignment honoured for power-of-two values up to the buffer's
    //    own base alignment (64B from _aligned_malloc / aligned_alloc).
    {
        FrameAllocator a(8192);
        a.allocate(1, 1); // bias offset
        for (size_t align : {16u, 32u, 64u}) {
            void* p = a.allocate(8, align);
            PT_ASSERT(p != nullptr, "aligned allocation");
            PT_ASSERT_OP(reinterpret_cast<uintptr_t>(p) % align, ==, 0u,
                         "alignment honoured");
        }
    }

    // 3. Out-of-space → nullptr.
    {
        FrameAllocator a(256);
        void* p = a.allocate(128, 16);
        PT_ASSERT(p != nullptr, "first 128B fits");
        void* q = a.allocate(256, 16); // overflows
        PT_ASSERT(q == nullptr, "OOS returns nullptr");
    }

    // 4. reset() rolls offset back to 0; same address reused.
    {
        FrameAllocator a(1024);
        void* before = a.allocate(64, 16);
        a.reset();
        PT_ASSERT_OP(a.used(), ==, 0u, "used == 0 after reset");
        void* after = a.allocate(64, 16);
        PT_ASSERT(after == before, "reset reuses same first slot");
    }

    // 5. Typed array helper.
    {
        FrameAllocator a(4096);
        auto* arr = a.allocate_array<uint64_t>(100);
        PT_ASSERT(arr != nullptr, "typed array");
        PT_ASSERT_OP(reinterpret_cast<uintptr_t>(arr) % alignof(uint64_t), ==, 0u,
                     "typed array align");
        for (int i = 0; i < 100; ++i) arr[i] = static_cast<uint64_t>(i);
        PT_ASSERT_OP(arr[42], ==, 42u, "writable");
    }

    // 6. Peak tracking.
    {
        FrameAllocator a(4096);
        a.allocate(1024, 16);
        size_t peak1 = a.peak();
        PT_ASSERT_OP(peak1, >=, 1024u, "peak >= 1024");
        a.reset();
        // peak is "since last reset" semantics; either resets or keeps —
        // we only require it to be at least the latest mark afterwards.
        a.allocate(64, 16);
        PT_ASSERT_OP(a.peak(), >=, 64u, "peak after reset");
    }

    return report("unit_frame_allocator_test");
}
