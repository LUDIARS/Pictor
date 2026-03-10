#pragma once

#include "pictor/memory/frame_allocator.h"
#include "pictor/memory/pool_allocator.h"
#include "pictor/memory/gpu_memory_allocator.h"

namespace pictor {

/// Central memory management (§4, §11).
/// Owns all allocators and provides unified access.
struct MemoryConfig {
    size_t   frame_allocator_size = 16 * 1024 * 1024; // 16MB per flight
    uint32_t flight_count         = 3;
    size_t   pool_chunk_size      = 64 * 1024;         // 64KB
    GpuMemoryAllocator::Config gpu_config;
    bool     use_large_pages      = false;
};

class MemorySubsystem {
public:
    MemorySubsystem();
    explicit MemorySubsystem(const MemoryConfig& config);
    ~MemorySubsystem();

    MemorySubsystem(const MemorySubsystem&) = delete;
    MemorySubsystem& operator=(const MemorySubsystem&) = delete;

    /// Frame lifecycle
    void begin_frame();
    void end_frame();

    /// Allocators
    FrameAllocator&     frame_allocator();
    PoolAllocator&      pool_allocator() { return pool_allocator_; }
    GpuMemoryAllocator& gpu_allocator()  { return gpu_allocator_; }

    /// Stats
    struct Stats {
        size_t frame_alloc_used     = 0;
        size_t frame_alloc_peak     = 0;
        size_t frame_alloc_capacity = 0;
        size_t pool_allocated       = 0;
        GpuMemoryAllocator::Stats gpu_stats;
    };

    Stats get_stats() const;
    uint32_t frame_number() const { return frame_number_; }

private:
    FlightFrameAllocator flight_allocator_;
    PoolAllocator        pool_allocator_;
    GpuMemoryAllocator   gpu_allocator_;
    MemoryConfig         config_;
    uint32_t             frame_number_ = 0;
};

} // namespace pictor
