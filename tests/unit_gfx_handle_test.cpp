// Generation-tagged gfx handles and the allocator behind them
// (spec/architecture/multi-rhi.md §1 公開面, §6 S0).

#include "pictor/gfx/handle.h"
#include "rhi/common/handle_allocator.h"
#include "test_common.h"

#include <type_traits>
#include <vector>

using namespace pictor_test;
using pictor::gfx::BufferId;
using pictor::gfx::TextureId;
using pictor::rhi::HandleAllocator;

static void test_handle_layout() {
    static_assert(sizeof(BufferId) == 4);
    static_assert(std::is_trivially_copyable_v<BufferId>);
    static_assert(!std::is_convertible_v<BufferId, TextureId>, "handle kinds do not mix");

    BufferId invalid{};
    PT_ASSERT(!invalid.valid(), "default handle is invalid");
    PT_ASSERT_OP(invalid.bits, ==, 0u, "default handle bits are 0");

    BufferId h = BufferId::make(1234, 7);
    PT_ASSERT_OP(h.index(), ==, 1234u, "index round-trips");
    PT_ASSERT_OP(h.generation(), ==, 7u, "generation round-trips");
    PT_ASSERT(h.valid(), "non-zero generation is valid");

    BufferId max = BufferId::make(BufferId::kMaxIndex, BufferId::kMaxGeneration);
    PT_ASSERT_OP(max.index(), ==, BufferId::kMaxIndex, "max index fits");
    PT_ASSERT_OP(max.generation(), ==, BufferId::kMaxGeneration, "max generation fits");
    PT_ASSERT(BufferId::make(3, 1) != BufferId::make(3, 2), "same slot, different generation differ");
}

static void test_allocate_release_reuse() {
    HandleAllocator a;
    const uint32_t h0 = a.allocate();
    const uint32_t h1 = a.allocate();
    PT_ASSERT(h0 != 0 && h1 != 0, "allocate issues valid handles");
    PT_ASSERT(h0 != h1, "two live handles differ");
    PT_ASSERT_OP(a.live_count(), ==, 2u, "two live");
    PT_ASSERT(a.is_alive(h0) && a.is_alive(h1), "both alive");

    PT_ASSERT(a.release(h0), "release live handle");
    PT_ASSERT(!a.is_alive(h0), "released handle is stale");
    PT_ASSERT(!a.release(h0), "double release is rejected");
    PT_ASSERT_OP(a.live_count(), ==, 1u, "one live after release");

    const uint32_t h2 = a.allocate();
    const BufferId old_h = BufferId::from_bits(h0);
    const BufferId new_h = BufferId::from_bits(h2);
    PT_ASSERT_OP(new_h.index(), ==, old_h.index(), "freed slot is reused");
    PT_ASSERT_OP(new_h.generation(), ==, old_h.generation() + 1, "reuse bumps the generation");
    PT_ASSERT(!a.is_alive(h0), "old handle stays stale after slot reuse");
    PT_ASSERT(a.is_alive(h2), "new handle alive");
    PT_ASSERT_OP(a.slot_count(), ==, 2u, "no new slot for reuse");
}

static void test_invalid_inputs() {
    HandleAllocator a;
    PT_ASSERT(!a.is_alive(0), "bits 0 never alive");
    PT_ASSERT(!a.release(0), "release of invalid handle rejected");
    PT_ASSERT(!a.is_alive(BufferId::make(5, 1).bits), "never-issued index not alive");
    const uint32_t h = a.allocate();
    PT_ASSERT(!a.is_alive(BufferId::make(BufferId::from_bits(h).index(), 2).bits),
              "wrong generation not alive");
}

static void test_generation_wraps_past_zero() {
    HandleAllocator a;
    uint32_t h = a.allocate();
    const uint32_t index = BufferId::from_bits(h).index();
    // Cycle one slot through every generation; it must wrap to 1, never 0.
    for (uint32_t i = 0; i < BufferId::kMaxGeneration; ++i) {
        PT_ASSERT(a.release(h), "release in wrap loop");
        h = a.allocate();
        PT_ASSERT_OP(BufferId::from_bits(h).index(), ==, index, "same slot in wrap loop");
        PT_ASSERT(BufferId::from_bits(h).generation() != 0, "generation 0 never issued");
        if (g_failures) return;
    }
    PT_ASSERT_OP(BufferId::from_bits(h).generation(), ==, 1u, "generation wrapped back to 1");
}

static void test_many_handles_unique() {
    HandleAllocator a;
    std::vector<uint32_t> live;
    for (int i = 0; i < 1000; ++i) live.push_back(a.allocate());
    for (size_t i = 0; i < live.size(); i += 2) a.release(live[i]);
    uint32_t alive = 0;
    for (size_t i = 0; i < live.size(); ++i) alive += a.is_alive(live[i]) ? 1u : 0u;
    PT_ASSERT_OP(alive, ==, 500u, "odd handles survive");
    PT_ASSERT_OP(a.live_count(), ==, 500u, "live_count matches");
}

int main() {
    test_handle_layout();
    test_allocate_release_reuse();
    test_invalid_inputs();
    test_generation_wraps_past_zero();
    test_many_handles_unique();
    return report("unit_gfx_handle_test");
}
