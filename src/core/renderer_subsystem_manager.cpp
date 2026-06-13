#include "pictor/core/renderer_subsystem_manager.h"
#include "pictor/core/pictor_renderer.h" // RendererConfig

namespace pictor {

RendererSubsystemManager::RendererSubsystemManager() = default;
RendererSubsystemManager::~RendererSubsystemManager() = default;

void RendererSubsystemManager::initialize(const RendererConfig& config) {
    screen_width_  = config.screen_width;
    screen_height_ = config.screen_height;

    // §12: 依存順に構築する (旧 PictorRenderer::initialize)。

    // 1. Memory Subsystem
    memory_ = std::make_unique<MemorySubsystem>(config.memory_config);

    // 2. Scene Registry
    scene_ = std::make_unique<SceneRegistry>(*memory_);

    // 3. Update Scheduler
    update_scheduler_ = std::make_unique<UpdateScheduler>(*scene_, config.update_config);

    // 4. Batch Builder
    batch_builder_ = std::make_unique<BatchBuilder>(*scene_);

    // 5. Culling System
    culling_ = std::make_unique<CullingSystem>(*scene_);

    // 6. GPU Buffer Manager
    gpu_buffer_manager_ = std::make_unique<GPUBufferManager>(memory_->gpu_allocator());

    // 7. Pipeline Profile Manager
    profile_manager_ = std::make_unique<PipelineProfileManager>();
    profile_manager_->register_defaults();

    // 8. Apply initial profile
    if (!config.initial_profile.empty()) {
        profile_manager_->set_profile(config.initial_profile);
    }
    const auto& profile = profile_manager_->current_profile();

    // 9. GPU Driven Pipeline (if enabled)
    if (profile.gpu_driven_enabled) {
        gpu_pipeline_ = std::make_unique<GPUDrivenPipeline>(
            *gpu_buffer_manager_, *scene_, profile.gpu_driven_config);
    }

    // 10. GI Lighting System (shadow maps + AO + probes)
    if (profile.gi_config.shadow_enabled || profile.gi_config.ssao_enabled ||
        profile.gi_config.gi_probes_enabled) {
        gi_system_ = std::make_unique<GILightingSystem>(
            *gpu_buffer_manager_, *scene_, profile.gi_config);
        gi_system_->initialize(
            profile.gpu_driven_config.max_triangle_count,
            config.screen_width, config.screen_height);

        // Bake system (depends on GI system)
        bake_system_ = std::make_unique<GIBakeSystem>(
            *gpu_buffer_manager_, *scene_, *gi_system_);
    }

    // 11. Render Pass Scheduler
    pass_scheduler_ = std::make_unique<RenderPassScheduler>(profile);

    // 12. Profiler
    profiler_ = std::make_unique<Profiler>();
    profiler_->set_enabled(config.profiler_enabled);
    profiler_->set_overlay_mode(config.overlay_mode);

    // 13. Overlay Renderer
    overlay_ = std::make_unique<OverlayRenderer>();
    overlay_->initialize(config.screen_width, config.screen_height);

    // 14. Stats Overlay
    stats_overlay_ = std::make_unique<StatsOverlay>();
    stats_overlay_->initialize(config.screen_width, config.screen_height);

    // 15. Data Exporter
    data_exporter_ = std::make_unique<DataExporter>();

    // 16. Animation System (before DataHandler, which depends on it)
    animation_system_ = std::make_unique<AnimationSystem>(AnimationSystemConfig{});

    // 17. Data Handler
    data_handler_ = std::make_unique<DataHandler>(
        memory_->gpu_allocator(), *gpu_buffer_manager_, *animation_system_);

    // 18. Post-Process は host-driven の `PostProcessPipeline` を使う
    //     (PictorRenderer は関与しない)。
}

void RendererSubsystemManager::shutdown() {
    // 構築の逆順で破棄する。
    animation_system_.reset();
    data_handler_.reset();
    data_exporter_.reset();
    stats_overlay_.reset();
    overlay_.reset();
    profiler_.reset();
    pass_scheduler_.reset();
    bake_system_.reset();
    gi_system_.reset();
    gpu_pipeline_.reset();
    profile_manager_.reset();
    gpu_buffer_manager_.reset();
    culling_.reset();
    batch_builder_.reset();
    update_scheduler_.reset();
    scene_.reset();
    memory_.reset();
}

void RendererSubsystemManager::apply_profile(const PipelineProfileDef& profile) {
    // §8.4: Profile switch procedure

    // 3. UpdateScheduler config change
    update_scheduler_->set_config(profile.update_config);

    // 4. Invalidate all batches
    batch_builder_->invalidate_all();

    // 5. RenderPassScheduler reconfigure
    pass_scheduler_->reconfigure(profile);

    // 6. GPU resource reallocation
    if (profile.gpu_driven_enabled && !gpu_pipeline_) {
        gpu_pipeline_ = std::make_unique<GPUDrivenPipeline>(
            *gpu_buffer_manager_, *scene_, profile.gpu_driven_config);
    } else if (!profile.gpu_driven_enabled) {
        gpu_pipeline_.reset();
    } else if (gpu_pipeline_) {
        gpu_pipeline_->set_config(profile.gpu_driven_config);
    }

    // 7. GI system reconfigure
    if (profile.gi_config.shadow_enabled || profile.gi_config.ssao_enabled ||
        profile.gi_config.gi_probes_enabled) {
        if (!gi_system_) {
            gi_system_ = std::make_unique<GILightingSystem>(
                *gpu_buffer_manager_, *scene_, profile.gi_config);
            gi_system_->initialize(
                profile.gpu_driven_config.max_triangle_count,
                screen_width_, screen_height_);
        } else {
            gi_system_->set_config(profile.gi_config);
        }
    } else {
        gi_system_.reset();
    }

    // 8. Profiler config
    profiler_->set_enabled(profile.profiler_config.enabled);
    profiler_->set_overlay_mode(profile.profiler_config.overlay_mode);
}

} // namespace pictor
