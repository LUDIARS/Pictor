#pragma once

#include "pictor/core/types.h"
#include "pictor/culling/flat_bvh.h"
#include "pictor/scene/scene_registry.h"
#include "pictor/memory/frame_allocator.h"

namespace pictor {

/// Custom culling algorithm interface (§12.2)
class ICullingProvider {
public:
    virtual ~ICullingProvider() = default;

    /// Perform culling, write results to visibility_flags
    virtual void cull(const AABB* bounds, uint8_t* visibility_flags,
                      uint32_t count, const Frustum& frustum) = 0;
};

/// Multi-level culling system (§10.1):
/// Level 1: CPU Frustum Culling via Flat BVH
/// Level 2: CPU Software Occlusion (optional)
/// Level 3: GPU Hi-Z Occlusion Culling (via compute)
class CullingSystem {
public:
    explicit CullingSystem(SceneRegistry& registry);
    ~CullingSystem();

    /// Build/rebuild BVH for static objects
    void build_static_bvh(PoolAllocator& allocator);

    /// Refit BVH for dynamic objects
    void refit_dynamic_bvh();

    /// Execute all culling levels for current frame
    void cull(const Frustum& frustum, FrameAllocator& frame_allocator);

    /// Set custom culling provider (§12.2)
    void set_culling_provider(ICullingProvider* provider) { custom_provider_ = provider; }

    /// Statistics
    struct Stats {
        uint32_t total_objects   = 0;
        uint32_t visible_objects = 0;
        uint32_t culled_objects  = 0;
        float    cull_ratio      = 0.0f;
    };

    Stats get_stats() const { return stats_; }

    const FlatBVH& static_bvh() const { return static_bvh_; }
    const FlatBVH& dynamic_bvh() const { return dynamic_bvh_; }

private:
    /// Level 1: CPU Frustum Culling (§10.1)
    void frustum_cull_pool(ObjectPool& pool, const Frustum& frustum,
                           FrameAllocator& allocator);

    /// Simple linear frustum cull (fallback when BVH is not available)
    void frustum_cull_linear(ObjectPool& pool, const Frustum& frustum);

    SceneRegistry&    registry_;
    FlatBVH           static_bvh_;
    FlatBVH           dynamic_bvh_;
    ICullingProvider*  custom_provider_ = nullptr;
    Stats             stats_;
};

} // namespace pictor
