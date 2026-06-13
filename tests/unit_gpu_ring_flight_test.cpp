/// GPU ring プール (instance / staging) の flight 多重化テスト
/// (review/2026-06-11 H-3)。
///
/// 不変条件: 連続するフライトの ring 確保は互いに重ならない区画に入り、
/// flight_count 回の reset で元の区画へ巡回する。 これにより GPU が前フレームの
/// コマンドバッファ経由で参照中の領域を begin_frame が上書きしないことを保証する。

#include "pictor/memory/gpu_memory_allocator.h"
#include "test_common.h"

using namespace pictor;
using namespace pictor_test;

namespace {

struct Range { size_t lo, hi; };
bool overlaps(const Range& a, const Range& b) {
    return a.lo < b.hi && b.lo < a.hi;
}

GpuMemoryAllocator::Config make_config(size_t per_flight, uint32_t flights) {
    GpuMemoryAllocator::Config c;
    c.instance_buffer_size = per_flight;
    c.staging_buffer_size  = per_flight;
    c.flight_count         = flights;
    return c;
}

// 3 フライト分の instance 確保が互いに非重複の区画に入ること。
void test_flights_do_not_overlap() {
    constexpr size_t kPerFlight = 1024;
    constexpr uint32_t kFlights = 3;
    GpuMemoryAllocator alloc(make_config(kPerFlight, kFlights));

    Range r[kFlights];
    for (uint32_t f = 0; f < kFlights; ++f) {
        auto a = alloc.allocate_instance(256, 16);
        PT_ASSERT(a.valid, "instance allocation must succeed within region");
        r[f] = {a.offset, a.offset + a.size};
        alloc.reset_ring_buffers(); // 次フライトへ前進
    }

    for (uint32_t i = 0; i < kFlights; ++i)
        for (uint32_t j = i + 1; j < kFlights; ++j)
            PT_ASSERT(!overlaps(r[i], r[j]),
                      "allocations in different flights must not overlap");
}

// flight_count 回 reset すると最初の区画へ巡回すること。
void test_wraps_after_flight_count() {
    constexpr size_t kPerFlight = 1024;
    constexpr uint32_t kFlights = 3;
    GpuMemoryAllocator alloc(make_config(kPerFlight, kFlights));

    auto first = alloc.allocate_instance(128, 16);
    for (uint32_t f = 0; f < kFlights; ++f) alloc.reset_ring_buffers(); // 1 周
    auto wrapped = alloc.allocate_instance(128, 16);

    PT_ASSERT(first.valid && wrapped.valid, "allocations valid");
    PT_ASSERT_OP(first.offset, ==, wrapped.offset,
                 "after flight_count resets, region base must repeat");
}

// 区画 (per-flight) を超える確保は失敗すること。
void test_region_overflow_fails() {
    GpuMemoryAllocator alloc(make_config(1024, 3));
    auto ok  = alloc.allocate_instance(1024, 16);
    auto bad = alloc.allocate_instance(1, 16); // 区画満杯
    PT_ASSERT(ok.valid, "exact-fit allocation succeeds");
    PT_ASSERT(!bad.valid, "over-region allocation must fail (no spill into next flight)");
}

} // namespace

int main() {
    test_flights_do_not_overlap();
    test_wraps_after_flight_count();
    test_region_overflow_fails();
    return report("unit_gpu_ring_flight_test");
}
