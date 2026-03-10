#include "pictor/culling/culling_system.h"
#include <cstring>

namespace pictor {

CullingSystem::CullingSystem(SceneRegistry& registry)
    : registry_(registry)
{
}

CullingSystem::~CullingSystem() = default;

void CullingSystem::build_static_bvh(PoolAllocator& allocator) {
    ObjectPool& pool = registry_.static_pool();
    if (pool.empty()) return;

    uint32_t count = pool.count();
    // Create index array
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

    // Level 1: CPU Frustum Culling (§10.1)

    // Cull static pool (using BVH if available)
    frustum_cull_pool(registry_.static_pool(), frustum, frame_allocator);

    // Cull dynamic pool (linear scan — BVH may be stale)
    frustum_cull_linear(registry_.dynamic_pool(), frustum);

    // GPU Driven pool is culled on GPU (Level 3, §10.1)
    // Set all visibility to 1 for GPU Driven objects (GPU will cull them)
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

void CullingSystem::frustum_cull_pool(ObjectPool& pool, const Frustum& frustum,
                                       FrameAllocator& allocator) {
    if (pool.empty()) return;

    uint32_t count = pool.count();
    stats_.total_objects += count;

    if (static_bvh_.empty()) {
        // Fallback to linear cull
        frustum_cull_linear(pool, frustum);
        return;
    }

    // Use BVH for hierarchical culling
    uint32_t* visible_indices = allocator.allocate_array<uint32_t>(count);
    if (!visible_indices) {
        frustum_cull_linear(pool, frustum);
        return;
    }

    // Clear visibility
    std::memset(pool.visibility_flags().data(), 0, count);

    // BVH query returns visible object indices
    uint32_t visible_count = static_bvh_.query_frustum(frustum, visible_indices, count);

    // Mark visible objects
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

    // Linear frustum test — traverses bounds[] only (§10.1)
    uint32_t visible = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (custom_provider_) {
            // Use custom culling provider
            custom_provider_->cull(bounds, visibility, count, frustum);
            // Count visible after custom cull
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
