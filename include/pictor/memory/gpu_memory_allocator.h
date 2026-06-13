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
        size_t instance_buffer_size = 64  * 1024 * 1024; // 64MB (per flight)
        size_t indirect_buffer_size = 16  * 1024 * 1024; // 16MB
        size_t staging_buffer_size  = 64  * 1024 * 1024; // 64MB (per flight)
        // ring プール (instance / staging) を多重化するフライト数。 各フライトは
        // 上記サイズを丸ごと専有する (合計確保 = size × flight_count)。
        // GPU が前フレームのコマンドバッファ経由で参照中の領域を begin_frame で
        // 上書きしないための保護 (review/2026-06-11 H-3)。
        uint32_t flight_count       = 3;
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
        std::vector<FreeBlock> free_list;   // 非 ring プール (mesh / ssbo) のみ使用。

        // ring プール (instance / staging) 用。 capacity を flight_count 等分し、
        // 各フライトの区画内を bump 確保する。 begin_frame では current_flight の
        // 区画のみリセットし、 他フライトが GPU 参照中の領域は温存する (H-3)。
        uint32_t flight_count   = 1;
        uint32_t current_flight = 0;
        size_t   region_size    = 0;   // = capacity / flight_count
        size_t   head           = 0;   // 現フライト区画内の bump 位置 (絶対 offset)
    };

    GpuAllocation allocate_from_pool(Pool& pool, size_t size, size_t alignment);
    GpuAllocation allocate_ring(Pool& pool, size_t size, size_t alignment);

    Pool mesh_pool_;
    Pool ssbo_pool_;
    Pool instance_pool_;
    Pool staging_pool_;
    uint64_t next_buffer_id_ = 1;
};

} // namespace pictor
