#pragma once

#include "pictor/core/types.h"
#include "pictor/gpu/gpu_buffer_manager.h"
#include "pictor/scene/scene_registry.h"
#include "pictor/update/update_scheduler.h"

namespace pictor {

/// GPU Driven config (§8.2)
struct GPUDrivenConfig {
    uint32_t max_triangle_count   = 50000;
    uint32_t min_instance_count   = 32;
    uint32_t workgroup_size       = 256;
    bool     two_phase_culling    = true;  // Hi-Z two-phase (§10.3)
    bool     compute_update       = true;  // Enable Compute Update (§5.4)
};

/// GPU Driven rendering pipeline (§7).
/// Handles Compute Update, Compute Culling, LOD selection,
/// Compact pass, and Indirect Draw generation — all on GPU.
class GPUDrivenPipeline {
public:
    GPUDrivenPipeline(GPUBufferManager& buffer_manager,
                      SceneRegistry& registry);
    GPUDrivenPipeline(GPUBufferManager& buffer_manager,
                      SceneRegistry& registry,
                      const GPUDrivenConfig& config);
    ~GPUDrivenPipeline();

    GPUDrivenPipeline(const GPUDrivenPipeline&) = delete;
    GPUDrivenPipeline& operator=(const GPUDrivenPipeline&) = delete;

    /// Initialize GPU resources for the pipeline
    void initialize(uint32_t max_objects);

    /// Execute the full GPU driven pipeline for a frame (§7.2)
    ///  Step 1: Compute Update (gpu_transforms/gpu_bounds)
    ///  Step 2: Compute Cull (Frustum + Hi-Z)
    ///  Step 3: LOD Select
    ///  Step 4: Compact + Indirect Draw generation
    void execute(const Frustum& frustum,
                 const UpdateScheduler::ComputeUpdateParams& params);

    /// Upload initial data to GPU SSBOs
    void upload_initial_data(const ObjectPool& pool);

    /// Get the SSBO layout for binding
    const GPUBufferManager::SSBOLayout& ssbo_layout() const { return ssbo_layout_; }

    /// Configuration
    void set_config(const GPUDrivenConfig& config) { config_ = config; }
    const GPUDrivenConfig& config() const { return config_; }

    /// Statistics
    struct Stats {
        uint32_t total_objects   = 0;
        uint32_t visible_objects = 0;
        uint32_t draw_calls      = 0;
        uint32_t workgroups      = 0;
    };

    Stats get_stats() const { return stats_; }

private:
    /// Compute shader dispatch helpers
    uint32_t calculate_workgroups(uint32_t count) const;

    GPUBufferManager&             buffer_manager_;
    SceneRegistry&                registry_;
    GPUDrivenConfig               config_;
    GPUBufferManager::SSBOLayout  ssbo_layout_;
    Stats                         stats_;
    bool                          initialized_ = false;
};

} // namespace pictor
