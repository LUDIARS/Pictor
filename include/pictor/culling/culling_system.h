#pragma once

#include "pictor/core/types.h"
#include "pictor/culling/flat_bvh.h"
#include "pictor/culling/world_partition.h"
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

/// Multi-level culling system (§10.1, §10.3):
///
/// Level 0: World Partition Broad Phase (§10.3)
///   - Grid-based spatial partition skips entire cells outside frustum
/// Level 1: CPU Frustum Culling via Flat BVH / Narrow Phase
///   - Per-object AABB-frustum test within active cells
/// Level 2: CPU Software Occlusion (optional)
/// Level 3: GPU Hi-Z Occlusion Culling (via compute)
class CullingSystem {
public:
    explicit CullingSystem(SceneRegistry& registry);
    ~CullingSystem();

    /// Configure world partition (call before first cull)
    void configure_partition(const WorldPartitionConfig& config);

    /// Rebuild world partition for a pool
    void rebuild_partition(PoolType pool_type);

    /// Build/rebuild BVH for static objects
    void build_static_bvh(PoolAllocator& allocator);

    /// Refit BVH for dynamic objects
    void refit_dynamic_bvh();

    /// Execute all culling levels for current frame
    void cull(const Frustum& frustum, FrameAllocator& frame_allocator);

    /// Set custom culling provider (§12.2)
    void set_culling_provider(ICullingProvider* provider) { custom_provider_ = provider; }

    /// Access world partition
    WorldPartition& partition() { return partition_; }
    const WorldPartition& partition() const { return partition_; }

    /// Statistics
    struct Stats {
        uint32_t total_objects   = 0;
        uint32_t visible_objects = 0;
        uint32_t culled_objects  = 0;
        uint32_t cells_tested    = 0;
        uint32_t cells_visible   = 0;
        float    cull_ratio      = 0.0f;
    };

    Stats get_stats() const { return stats_; }

    bool partition_enabled() const { return partition_enabled_; }

    const FlatBVH& static_bvh() const { return static_bvh_; }
    const FlatBVH& dynamic_bvh() const { return dynamic_bvh_; }

private:
    /// Level 0+1: Partition broad phase → frustum narrow phase
    void partition_cull_pool(ObjectPool& pool, const Frustum& frustum,
                              FrameAllocator& allocator);

    /// Level 1: CPU Frustum Culling via BVH (§10.1)
    void frustum_cull_pool(ObjectPool& pool, const Frustum& frustum,
                           FrameAllocator& allocator);

    /// Simple linear frustum cull (fallback when BVH is not available)
    void frustum_cull_linear(ObjectPool& pool, const Frustum& frustum);

    SceneRegistry&    registry_;
    WorldPartition    partition_;
    bool              partition_enabled_ = false;
    FlatBVH           static_bvh_;
    FlatBVH           dynamic_bvh_;
    ICullingProvider*  custom_provider_ = nullptr;
    Stats             stats_;
};

} // namespace pictor
