#include "pictor/culling/culling_system.h"
#include <cstring>
#include <cstdio>

namespace pictor {

CullingSystem::CullingSystem(SceneRegistry& registry)
    : registry_(registry)
{
}

CullingSystem::~CullingSystem() = default;

void CullingSystem::configure_partition(const WorldPartitionConfig& config) {
    partition_.configure(config);
    partition_enabled_ = true;
}

void CullingSystem::rebuild_partition(PoolType pool_type) {
    if (!partition_enabled_) return;

    ObjectPool* pool = nullptr;
    switch (pool_type) {
        case PoolType::STATIC:  pool = &registry_.static_pool(); break;
        case PoolType::DYNAMIC: pool = &registry_.dynamic_pool(); break;
        default: return;
    }

    if (pool->empty()) return;
    partition_.rebuild(pool->bounds().data(), pool->count());
}

void CullingSystem::build_static_bvh(PoolAllocator& allocator) {
    ObjectPool& pool = registry_.static_pool();
    if (pool.empty()) return;

    uint32_t count = pool.count();
    uint32_t* indices = static_cast<uint32_t*>(
        allocator.allocate(count * sizeof(uint32_t)));
    for (uint32_t i = 0; i < count; ++i) {
        indices[i] = i;
    }

    static_bvh_.build(pool.bounds().data(), indices, count, allocator);
}

void CullingSystem::refit_dynamic_bvh() {
    ObjectPool& pool = registry_.dynamic_pool();
    if (pool.empty()) return;

    dynamic_bvh_.refit(pool.bounds().data());
}

void CullingSystem::cull(const Frustum& frustum, FrameAllocator& frame_allocator) {
    stats_ = {};

    // ── Level 0+1: World Partition Broad Phase + Frustum Narrow Phase ──

    if (partition_enabled_ && partition_.cell_count() > 0) {
        // Static pool: partition broad phase → per-object narrow phase
        partition_cull_pool(registry_.static_pool(), frustum, frame_allocator);

        // Dynamic pool: also use partition if objects are assigned
        // (Dynamic objects should be re-assigned each frame via assign_object)
        partition_cull_pool(registry_.dynamic_pool(), frustum, frame_allocator);
    } else {
        // Fallback: original BVH / linear culling
        frustum_cull_pool(registry_.static_pool(), frustum, frame_allocator);
        frustum_cull_linear(registry_.dynamic_pool(), frustum);
    }

    // GPU Driven pool is culled on GPU (Level 3, §10.1)
    ObjectPool& gpu_pool = registry_.gpu_driven_pool();
    if (!gpu_pool.empty()) {
        std::memset(gpu_pool.visibility_flags().data(), 1, gpu_pool.count());
        stats_.total_objects += gpu_pool.count();
        stats_.visible_objects += gpu_pool.count();
    }

    if (stats_.total_objects > 0) {
        stats_.culled_objects = stats_.total_objects - stats_.visible_objects;
        stats_.cull_ratio = static_cast<float>(stats_.culled_objects) /
                            static_cast<float>(stats_.total_objects);
    }
}

// ── Level 0+1: Partition Broad Phase + Frustum Narrow Phase ──────

void CullingSystem::partition_cull_pool(ObjectPool& pool, const Frustum& frustum,
                                         FrameAllocator& allocator) {
    if (pool.empty()) return;

    uint32_t count = pool.count();
    stats_.total_objects += count;

    // Clear all visibility
    std::memset(pool.visibility_flags().data(), 0, count);

    const AABB* bounds = pool.bounds().data();
    uint8_t* visibility = pool.visibility_flags().data();

    // Broad phase: query partition for cells that intersect frustum
    uint32_t max_cells = partition_.cell_count();
    if (max_cells == 0) {
        // No cells — fall back to linear
        frustum_cull_linear(pool, frustum);
        return;
    }

    const PartitionCell** active_cells = allocator.allocate_array<const PartitionCell*>(max_cells);
    if (!active_cells) {
        frustum_cull_linear(pool, frustum);
        return;
    }

    stats_.cells_tested += max_cells;
    uint32_t active_count = partition_.query_frustum(frustum, active_cells, max_cells);
    stats_.cells_visible += active_count;

    // Narrow phase: per-object frustum test within active cells only
    uint32_t visible = 0;
    for (uint32_t c = 0; c < active_count; ++c) {
        const PartitionCell* cell = active_cells[c];
        for (uint32_t idx : cell->object_indices) {
            if (idx < count && frustum.test_aabb(bounds[idx])) {
                visibility[idx] = 1;
                ++visible;
            }
        }
    }

    stats_.visible_objects += visible;
}

// ── Level 1: BVH Frustum Culling (original) ─────────────────────

void CullingSystem::frustum_cull_pool(ObjectPool& pool, const Frustum& frustum,
                                       FrameAllocator& allocator) {
    if (pool.empty()) return;

    uint32_t count = pool.count();
    stats_.total_objects += count;

    if (static_bvh_.empty()) {
        frustum_cull_linear(pool, frustum);
        return;
    }

    uint32_t* visible_indices = allocator.allocate_array<uint32_t>(count);
    if (!visible_indices) {
        frustum_cull_linear(pool, frustum);
        return;
    }

    std::memset(pool.visibility_flags().data(), 0, count);

    uint32_t visible_count = static_bvh_.query_frustum(frustum, visible_indices, count);

    for (uint32_t i = 0; i < visible_count; ++i) {
        uint32_t idx = visible_indices[i];
        if (idx < count) {
            pool.visibility_flags()[idx] = 1;
        }
    }

    stats_.visible_objects += visible_count;
}

void CullingSystem::frustum_cull_linear(ObjectPool& pool, const Frustum& frustum) {
    if (pool.empty()) return;

    uint32_t count = pool.count();
    const AABB* bounds = pool.bounds().data();
    uint8_t* visibility = pool.visibility_flags().data();

    stats_.total_objects += count;

    uint32_t visible = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (custom_provider_) {
            custom_provider_->cull(bounds, visibility, count, frustum);
            for (uint32_t j = 0; j < count; ++j) {
                if (visibility[j]) ++visible;
            }
            stats_.visible_objects += visible;
            return;
        }

        if (frustum.test_aabb(bounds[i])) {
            visibility[i] = 1;
            ++visible;
        } else {
            visibility[i] = 0;
        }
    }

    stats_.visible_objects += visible;
}

} // namespace pictor
