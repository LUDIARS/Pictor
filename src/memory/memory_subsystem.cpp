#include "pictor/memory/memory_subsystem.h"

namespace pictor {

namespace {
// MemoryConfig.flight_count を GPU allocator の ring 多重化数へ伝播させる
// (両者が divergence しないよう単一ソースに揃える)。
GpuMemoryAllocator::Config with_flight(const MemoryConfig& c) {
    GpuMemoryAllocator::Config g = c.gpu_config;
    g.flight_count = c.flight_count;
    return g;
}
} // namespace

MemorySubsystem::MemorySubsystem() : MemorySubsystem(MemoryConfig{}) {}

MemorySubsystem::MemorySubsystem(const MemoryConfig& config)
    : flight_allocator_(config.frame_allocator_size, config.flight_count)
    , pool_allocator_(config.pool_chunk_size)
    , gpu_allocator_(with_flight(config))
    , config_(config)
{
}

MemorySubsystem::~MemorySubsystem() = default;

void MemorySubsystem::begin_frame() {
    // Advance flight and reset the new frame's allocator (§11.3 step 1)
    flight_allocator_.advance_frame();
    // Reset GPU ring buffers (instance, staging)
    gpu_allocator_.reset_ring_buffers();
    ++frame_number_;
}

void MemorySubsystem::end_frame() {
    // Nothing to do — frame allocator will be reset on next begin_frame
}

FrameAllocator& MemorySubsystem::frame_allocator() {
    return flight_allocator_.current();
}

MemorySubsystem::Stats MemorySubsystem::get_stats() const {
    Stats stats;
    auto& fa = const_cast<FlightFrameAllocator&>(flight_allocator_).current();
    stats.frame_alloc_used     = fa.used();
    stats.frame_alloc_peak     = fa.peak();
    stats.frame_alloc_capacity = fa.capacity();
    stats.pool_allocated       = pool_allocator_.total_allocated();
    stats.gpu_stats            = gpu_allocator_.get_stats();
    return stats;
}

} // namespace pictor
