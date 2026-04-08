#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>

namespace pictor {

/// GPU memory allocation handle
struct GpuAllocation {
    uint64_t buffer_id = 0;
    size_t   offset    = 0;
    size_t   size      = 0;
    bool     valid     = false;
};

/// Sub-allocation strategy for GPU buffers (§11.2).
/// Manages mesh pool, SoA SSBOs, instance buffers, and indirect draw buffers.
class GpuMemoryAllocator {
public:
    struct Config {
        size_t mesh_pool_size       = 256 * 1024 * 1024; // 256MB
        size_t ssbo_pool_size       = 128 * 1024 * 1024; // 128MB
        size_t instance_buffer_size = 64  * 1024 * 1024; // 64MB
        size_t indirect_buffer_size = 16  * 1024 * 1024; // 16MB
        size_t staging_buffer_size  = 64  * 1024 * 1024; // 64MB
    };

    GpuMemoryAllocator();
    explicit GpuMemoryAllocator(const Config& config);
    ~GpuMemoryAllocator();

    GpuMemoryAllocator(const GpuMemoryAllocator&) = delete;
    GpuMemoryAllocator& operator=(const GpuMemoryAllocator&) = delete;

    /// Allocate from mesh pool (VB/IB)
    GpuAllocation allocate_mesh(size_t size, size_t alignment = 256);

    /// Allocate SSBO region
    GpuAllocation allocate_ssbo(size_t size, size_t alignment = 256);

    /// Allocate instance buffer region (ring buffer)
    GpuAllocation allocate_instance(size_t size, size_t alignment = 16);

    /// Allocate staging buffer for CPU→GPU upload
    GpuAllocation allocate_staging(size_t size, size_t alignment = 16);

    /// Free a specific allocation
    void free(const GpuAllocation& alloc);

    /// Reset ring buffers (instance, staging) at frame boundary
    void reset_ring_buffers();

    /// Statistics
    struct Stats {
        size_t mesh_pool_used       = 0;
        size_t mesh_pool_capacity   = 0;
        size_t ssbo_used            = 0;
        size_t ssbo_capacity        = 0;
        size_t instance_used        = 0;
        size_t instance_capacity    = 0;
        size_t staging_used         = 0;
        size_t staging_capacity     = 0;
        float  mesh_fragmentation   = 0.0f;
    };

    Stats get_stats() const;

private:
    struct Pool {
        uint64_t buffer_id = 0;
        size_t   capacity  = 0;
        size_t   used      = 0;
        bool     is_ring   = false;

        struct FreeBlock {
            size_t offset;
            size_t size;
        };
        std::vector<FreeBlock> free_list;
    };

    GpuAllocation allocate_from_pool(Pool& pool, size_t size, size_t alignment);

    Pool mesh_pool_;
    Pool ssbo_pool_;
    Pool instance_pool_;
    Pool staging_pool_;
    uint64_t next_buffer_id_ = 1;
};

} // namespace pictor
