#include "pictor/memory/gpu_memory_allocator.h"
#include <algorithm>

namespace pictor {

GpuMemoryAllocator::GpuMemoryAllocator() : GpuMemoryAllocator(Config{}) {}

GpuMemoryAllocator::GpuMemoryAllocator(const Config& config) {
    // Initialize pools with free list
    auto init_pool = [this](Pool& pool, size_t capacity, bool is_ring) {
        pool.buffer_id = next_buffer_id_++;
        pool.capacity = capacity;
        pool.used = 0;
        pool.is_ring = is_ring;
        pool.free_list.push_back({0, capacity});
    };

    init_pool(mesh_pool_, config.mesh_pool_size, false);
    init_pool(ssbo_pool_, config.ssbo_pool_size, false);
    init_pool(instance_pool_, config.instance_buffer_size, true);
    init_pool(staging_pool_, config.staging_buffer_size, true);
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

GpuAllocation GpuMemoryAllocator::allocate_instance(size_t size, size_t alignment) {
    return allocate_from_pool(instance_pool_, size, alignment);
}

GpuAllocation GpuMemoryAllocator::allocate_staging(size_t size, size_t alignment) {
    return allocate_from_pool(staging_pool_, size, alignment);
}

void GpuMemoryAllocator::free(const GpuAllocation& alloc) {
    if (!alloc.valid) return;

    Pool* pool = nullptr;
    if (alloc.buffer_id == mesh_pool_.buffer_id) pool = &mesh_pool_;
    else if (alloc.buffer_id == ssbo_pool_.buffer_id) pool = &ssbo_pool_;
    else if (alloc.buffer_id == instance_pool_.buffer_id) pool = &instance_pool_;
    else if (alloc.buffer_id == staging_pool_.buffer_id) pool = &staging_pool_;
    else return;

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
    // Ring buffers reset each frame (§11.2)
    auto reset_pool = [](Pool& pool) {
        pool.used = 0;
        pool.free_list.clear();
        pool.free_list.push_back({0, pool.capacity});
    };

    reset_pool(instance_pool_);
    reset_pool(staging_pool_);
}

GpuMemoryAllocator::Stats GpuMemoryAllocator::get_stats() const {
    Stats stats;
    stats.mesh_pool_used     = mesh_pool_.used;
    stats.mesh_pool_capacity = mesh_pool_.capacity;
    stats.ssbo_used          = ssbo_pool_.used;
    stats.ssbo_capacity      = ssbo_pool_.capacity;
    stats.instance_used      = instance_pool_.used;
    stats.instance_capacity  = instance_pool_.capacity;
    stats.staging_used       = staging_pool_.used;
    stats.staging_capacity   = staging_pool_.capacity;

    // Calculate fragmentation: ratio of free blocks to ideal (1 block)
    if (mesh_pool_.free_list.size() > 1) {
        stats.mesh_fragmentation =
            1.0f - 1.0f / static_cast<float>(mesh_pool_.free_list.size());
    }

    return stats;
}

} // namespace pictor
