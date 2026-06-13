#include "pictor/memory/gpu_memory_allocator.h"
#include <algorithm>

namespace pictor {

GpuMemoryAllocator::GpuMemoryAllocator() : GpuMemoryAllocator(Config{}) {}

GpuMemoryAllocator::GpuMemoryAllocator(const Config& config) {
    const uint32_t flights = config.flight_count > 0 ? config.flight_count : 1;

    // 非 ring プール (mesh / ssbo): 従来通り free-list 管理。
    auto init_free_pool = [this](Pool& pool, size_t capacity) {
        pool.buffer_id = next_buffer_id_++;
        pool.capacity  = capacity;
        pool.used      = 0;
        pool.is_ring   = false;
        pool.free_list.push_back({0, capacity});
    };

    // ring プール (instance / staging): per-flight サイズ × flight_count を確保し、
    // フライトごとに独立した区画を bump 確保する (H-3: in-flight GPU 参照保護)。
    auto init_ring_pool = [this, flights](Pool& pool, size_t per_flight_size) {
        pool.buffer_id     = next_buffer_id_++;
        pool.region_size   = per_flight_size;
        pool.flight_count  = flights;
        pool.capacity      = per_flight_size * flights;
        pool.current_flight = 0;
        pool.head          = 0;          // flight 0 区画の先頭
        pool.used          = 0;
        pool.is_ring       = true;
    };

    init_free_pool(mesh_pool_, config.mesh_pool_size);
    init_free_pool(ssbo_pool_, config.ssbo_pool_size);
    init_ring_pool(instance_pool_, config.instance_buffer_size);
    init_ring_pool(staging_pool_, config.staging_buffer_size);
}

GpuMemoryAllocator::~GpuMemoryAllocator() = default;

GpuAllocation GpuMemoryAllocator::allocate_from_pool(Pool& pool, size_t size, size_t alignment) {
    // First-fit with alignment
    for (auto it = pool.free_list.begin(); it != pool.free_list.end(); ++it) {
        size_t aligned_offset = (it->offset + alignment - 1) & ~(alignment - 1);
        size_t padding = aligned_offset - it->offset;
        size_t total_needed = padding + size;

        if (total_needed <= it->size) {
            GpuAllocation alloc;
            alloc.buffer_id = pool.buffer_id;
            alloc.offset = aligned_offset;
            alloc.size = size;
            alloc.valid = true;

            // Update free block
            if (padding > 0) {
                // Keep the padding as a free block
                Pool::FreeBlock front{it->offset, padding};
                it->offset = aligned_offset + size;
                it->size -= total_needed;
                if (it->size == 0) {
                    *it = front;
                    if (front.size == 0) {
                        pool.free_list.erase(it);
                    }
                } else {
                    pool.free_list.insert(it, front);
                }
            } else {
                it->offset += size;
                it->size -= size;
                if (it->size == 0) {
                    pool.free_list.erase(it);
                }
            }

            pool.used += size;
            return alloc;
        }
    }

    return {}; // allocation failed
}

GpuAllocation GpuMemoryAllocator::allocate_mesh(size_t size, size_t alignment) {
    return allocate_from_pool(mesh_pool_, size, alignment);
}

GpuAllocation GpuMemoryAllocator::allocate_ssbo(size_t size, size_t alignment) {
    return allocate_from_pool(ssbo_pool_, size, alignment);
}

GpuAllocation GpuMemoryAllocator::allocate_ring(Pool& pool, size_t size, size_t alignment) {
    // 現フライト区画 [base, base + region_size) 内で bump 確保。
    const size_t base    = static_cast<size_t>(pool.current_flight) * pool.region_size;
    const size_t aligned = (pool.head + alignment - 1) & ~(alignment - 1);
    if (aligned + size > base + pool.region_size) {
        return {}; // 現フライト区画が満杯
    }
    GpuAllocation alloc;
    alloc.buffer_id = pool.buffer_id;
    alloc.offset    = aligned;
    alloc.size      = size;
    alloc.valid     = true;

    pool.head = aligned + size;
    pool.used = pool.head - base;
    return alloc;
}

GpuAllocation GpuMemoryAllocator::allocate_instance(size_t size, size_t alignment) {
    return allocate_ring(instance_pool_, size, alignment);
}

GpuAllocation GpuMemoryAllocator::allocate_staging(size_t size, size_t alignment) {
    return allocate_ring(staging_pool_, size, alignment);
}

void GpuMemoryAllocator::free(const GpuAllocation& alloc) {
    if (!alloc.valid) return;

    Pool* pool = nullptr;
    if (alloc.buffer_id == mesh_pool_.buffer_id) pool = &mesh_pool_;
    else if (alloc.buffer_id == ssbo_pool_.buffer_id) pool = &ssbo_pool_;
    else if (alloc.buffer_id == instance_pool_.buffer_id) pool = &instance_pool_;
    else if (alloc.buffer_id == staging_pool_.buffer_id) pool = &staging_pool_;
    else return;

    // ring プールは個別 free しない (フライト区画ごと reset_ring_buffers で一括回収)。
    if (pool->is_ring) return;

    pool->used -= alloc.size;

    // Insert into free list and merge adjacent blocks
    Pool::FreeBlock block{alloc.offset, alloc.size};

    auto it = pool->free_list.begin();
    while (it != pool->free_list.end() && it->offset < block.offset) {
        ++it;
    }

    it = pool->free_list.insert(it, block);

    // Merge with next
    auto next = std::next(it);
    if (next != pool->free_list.end() && it->offset + it->size == next->offset) {
        it->size += next->size;
        pool->free_list.erase(next);
    }

    // Merge with previous
    if (it != pool->free_list.begin()) {
        auto prev = std::prev(it);
        if (prev->offset + prev->size == it->offset) {
            prev->size += it->size;
            pool->free_list.erase(it);
        }
    }
}

void GpuMemoryAllocator::reset_ring_buffers() {
    // フライトを 1 つ進め、 新しいフライトの区画だけをリセットする (§11.2 / H-3)。
    // 直前 (N-1) / N-2 フライトの区画は GPU が参照中の可能性があるため温存。
    auto advance_pool = [](Pool& pool) {
        pool.current_flight = (pool.current_flight + 1) % pool.flight_count;
        pool.head = static_cast<size_t>(pool.current_flight) * pool.region_size;
        pool.used = 0;
    };

    advance_pool(instance_pool_);
    advance_pool(staging_pool_);
}

GpuMemoryAllocator::Stats GpuMemoryAllocator::get_stats() const {
    Stats stats;
    stats.mesh_pool_used     = mesh_pool_.used;
    stats.mesh_pool_capacity = mesh_pool_.capacity;
    stats.ssbo_used          = ssbo_pool_.used;
    stats.ssbo_capacity      = ssbo_pool_.capacity;
    // ring プールの capacity は「現フライトが使える区画サイズ」を報告する
    // (used / capacity 比を意味あるものに保つ)。
    stats.instance_used      = instance_pool_.used;
    stats.instance_capacity  = instance_pool_.region_size;
    stats.staging_used       = staging_pool_.used;
    stats.staging_capacity   = staging_pool_.region_size;

    // Calculate fragmentation: ratio of free blocks to ideal (1 block)
    if (mesh_pool_.free_list.size() > 1) {
        stats.mesh_fragmentation =
            1.0f - 1.0f / static_cast<float>(mesh_pool_.free_list.size());
    }

    return stats;
}

} // namespace pictor
