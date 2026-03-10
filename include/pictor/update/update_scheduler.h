#pragma once

#include "pictor/core/types.h"
#include "pictor/update/job_dispatcher.h"
#include "pictor/scene/scene_registry.h"

namespace pictor {

/// User-provided update callback interface (§5.2, §12.2)
class IUpdateCallback {
public:
    virtual ~IUpdateCallback() = default;

    /// Called per job chunk. Update transforms and bounds in-place.
    virtual void update(float4x4* transforms, AABB* bounds,
                        uint32_t start, uint32_t count, float delta_time) = 0;
};

/// Update configuration (§8.2)
struct UpdateConfig {
    uint32_t chunk_size         = 16384; // 16K objects per job
    uint32_t worker_threads     = 0;     // 0 = auto-detect (cores - 1)
    bool     nt_store_enabled   = true;  // Non-Temporal Store for large batches
    uint32_t nt_store_threshold = 10000; // Min objects to enable NT store
};

/// Data update strategy selector and executor (§5, §2.2).
/// Automatically selects the optimal update strategy based on pool type and object count.
class UpdateScheduler {
public:
    explicit UpdateScheduler(SceneRegistry& registry);
    UpdateScheduler(SceneRegistry& registry, const UpdateConfig& config);
    ~UpdateScheduler();

    UpdateScheduler(const UpdateScheduler&) = delete;
    UpdateScheduler& operator=(const UpdateScheduler&) = delete;

    /// Set the update callback for CPU-side updates
    void set_update_callback(IUpdateCallback* callback) { callback_ = callback; }

    /// Execute data update for the current frame (§5.5 auto-selection)
    void update(float delta_time);

    /// Get the strategy that was selected for each pool
    UpdateStrategy strategy_for(PoolType type) const { return strategies_[static_cast<int>(type)]; }

    /// Replace the job dispatcher (§5.2 Design Note)
    void set_job_dispatcher(IJobDispatcher* dispatcher);

    /// Update configuration
    void set_config(const UpdateConfig& config) { config_ = config; }
    const UpdateConfig& config() const { return config_; }

    /// Frame parameters for GPU Compute Update (§5.4)
    struct ComputeUpdateParams {
        float delta_time   = 0.0f;
        float total_time   = 0.0f;
        uint32_t frame_number = 0;
        float gravity      = -9.81f;
    };

    const ComputeUpdateParams& compute_params() const { return compute_params_; }

private:
    /// Select strategy for a pool (§5.5)
    UpdateStrategy select_strategy(PoolType type, uint32_t object_count) const;

    /// Execute Level 1: Multi-threaded parallel + SoA range split (§5.2)
    void update_cpu_parallel(ObjectPool& pool, float delta_time);

    /// Execute Level 2: NT Store writes (§5.3)
    void update_cpu_parallel_nt(ObjectPool& pool, float delta_time);

    /// Prepare Level 3: GPU Compute Update params (§5.4)
    void prepare_compute_update(float delta_time);

    /// Copy current transforms to prevTransforms for motion vectors
    void copy_prev_transforms(ObjectPool& pool);

    SceneRegistry&       registry_;
    UpdateConfig         config_;
    IUpdateCallback*     callback_ = nullptr;
    UpdateStrategy       strategies_[3] = {};
    ComputeUpdateParams  compute_params_;
    float                total_time_ = 0.0f;

    std::unique_ptr<ThreadPoolDispatcher> default_dispatcher_;
    IJobDispatcher*                       dispatcher_ = nullptr;
};

} // namespace pictor
