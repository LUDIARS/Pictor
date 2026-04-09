#include "pictor/core/pictor_renderer.h"

namespace pictor {

PictorRenderer::PictorRenderer() = default;
PictorRenderer::~PictorRenderer() {
    if (initialized_) shutdown();
}

void PictorRenderer::initialize() { initialize(RendererConfig{}); }

void PictorRenderer::initialize(const RendererConfig& config) {
    config_ = config;

    // §12: Initialize backend, memory, profiler, default profile

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

    // 11. Command Encoder
    command_encoder_ = std::make_unique<CommandEncoder>();

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

    // 18. Post-Process Pipeline
    postprocess_ = std::make_unique<PostProcessPipeline>();
    {
        // Build default post-process config from profile's post_process_stack
        PostProcessConfig pp_config;
        // Default HDR config
        pp_config.hdr.enabled = true;
        pp_config.hdr.exposure = 1.0f;
        pp_config.hdr.gamma = 2.2f;

        // Enable effects based on profile stack
        pp_config.bloom.enabled = false;
        pp_config.tone_mapping.enabled = false;
        pp_config.depth_of_field.enabled = false;
        pp_config.gaussian_blur.enabled = false;

        for (const auto& pp_def : profile.post_process_stack) {
            if (pp_def.name == "Bloom")        pp_config.bloom.enabled = pp_def.enabled;
            if (pp_def.name == "Tonemapping")  pp_config.tone_mapping.enabled = pp_def.enabled;
        }

        postprocess_->initialize(config.screen_width, config.screen_height, pp_config);
    }

    initialized_ = true;
}

void PictorRenderer::shutdown() {
    if (!initialized_) return;

    // §12: Release all resources, GPU sync
    postprocess_.reset();
    animation_system_.reset();
    data_handler_.reset();
    data_exporter_.reset();
    stats_overlay_.reset();
    overlay_.reset();
    profiler_.reset();
    command_encoder_.reset();
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

    initialized_ = false;
}

void PictorRenderer::begin_frame(float delta_time) {
    if (!initialized_) return;

    delta_time_ = delta_time;
    ++frame_number_;

    // §12, §11.3: Fence wait + Frame Allocator reset + Profiler frame start
    memory_->begin_frame();
    gpu_buffer_manager_->reset_frame_buffers();

    profiler_->begin_frame();
    command_encoder_->reset();
}

void PictorRenderer::render(const Camera& camera) {
    if (!initialized_) return;

    auto& frame_alloc = memory_->frame_allocator();

    // §11.3 Step 2a: Animation Update
    profiler_->begin_cpu_section("AnimationUpdate");
    if (animation_system_) {
        animation_system_->update(delta_time_);
    }
    profiler_->end_cpu_section("AnimationUpdate");

    // §11.3 Step 2b: Data Update
    profiler_->begin_cpu_section("DataUpdate");
    update_scheduler_->update(delta_time_);
    profiler_->end_cpu_section("DataUpdate");

    // §11.3 Step 3: Culling
    profiler_->begin_cpu_section("Culling");
    culling_->cull(camera.frustum, frame_alloc);
    profiler_->end_cpu_section("Culling");

    // Record culling stats
    auto cull_stats = culling_->get_stats();
    profiler_->record_visible(cull_stats.visible_objects, cull_stats.culled_objects);

    // §11.3 Step 4: Sort + Step 5: Batch Build
    profiler_->begin_cpu_section("Sort");
    profiler_->begin_cpu_section("BatchBuild");
    batch_builder_->build(frame_alloc);
    profiler_->end_cpu_section("BatchBuild");
    profiler_->end_cpu_section("Sort");

    profiler_->record_batches(static_cast<uint32_t>(batch_builder_->batches().size()));

    // GPU Driven Pipeline execution (§7.2)
    if (gpu_pipeline_) {
        profiler_->begin_gpu_section("ComputeUpdate");
        profiler_->begin_gpu_section("GPUCullPass");
        gpu_pipeline_->execute(camera.frustum, update_scheduler_->compute_params());
        profiler_->end_gpu_section("GPUCullPass");
        profiler_->end_gpu_section("ComputeUpdate");

        auto gpu_stats = gpu_pipeline_->get_stats();
        profiler_->record_gpu_driven_objects(gpu_stats.total_objects);
        profiler_->record_compute_update_objects(gpu_stats.total_objects);
    }

    // GI pre-passes: shadow maps + SSAO + probe sampling
    // Executes after culling (visibility data ready), before command encoding.
    // Results are bound as read-only resources for material shaders.
    if (gi_system_) {
        profiler_->begin_gpu_section("GILighting");
        gi_system_->execute(camera.view, camera.projection);
        profiler_->end_gpu_section("GILighting");
    }

    // §11.3 Step 5-6: Encode + Submit
    profiler_->begin_cpu_section("CommandEncode");
    pass_scheduler_->execute(*batch_builder_, *culling_, gpu_pipeline_.get());
    profiler_->end_cpu_section("CommandEncode");

    profiler_->record_draw_calls(command_encoder_->draw_call_count());
    profiler_->record_triangles(command_encoder_->triangle_count());

    // Record memory stats
    auto mem_stats = memory_->get_stats();
    profiler_->record_memory_stats(
        mem_stats.frame_alloc_used, mem_stats.frame_alloc_capacity,
        mem_stats.pool_allocated,
        mem_stats.gpu_stats.ssbo_used, mem_stats.gpu_stats.mesh_pool_used,
        mem_stats.gpu_stats.ssbo_used + mem_stats.gpu_stats.mesh_pool_used +
        mem_stats.gpu_stats.instance_used,
        mem_stats.gpu_stats.ssbo_capacity + mem_stats.gpu_stats.mesh_pool_capacity
    );

    // Post-process pipeline execution
    if (postprocess_ && postprocess_->is_initialized() &&
        postprocess_->enabled_effect_count() > 0) {
        profiler_->begin_gpu_section("PostProcess");
        // In production: scene_color/depth/output would be actual texture handles
        // from the framebuffer manager. Here we use placeholders.
        postprocess_->execute(0, 1, 2, delta_time_);
        profiler_->end_gpu_section("PostProcess");
    }

    // Render profiler overlay (§13.6)
    if (profiler_->is_enabled() && profiler_->overlay_mode() != OverlayMode::OFF) {
        overlay_->render(profiler_->overlay_mode(),
                         profiler_->get_frame_stats(), *profiler_);
    }

    // Render stats overlay (S key toggle)
    if (stats_overlay_ && stats_overlay_->is_visible()) {
        stats_overlay_->render(profiler_->get_frame_stats(), get_scene_summary());
    }
}

void PictorRenderer::end_frame() {
    if (!initialized_) return;

    // §12, §11.3: Present
    profiler_->end_frame();
    memory_->end_frame();

    // Record frame for export if recording
    if (data_exporter_ && data_exporter_->is_recording()) {
        data_exporter_->record_frame(profiler_->get_frame_stats(), frame_number_);
    }
}

// ---- Object Operations ----

ObjectId PictorRenderer::register_object(const ObjectDescriptor& desc) {
    if (!initialized_) return INVALID_OBJECT_ID;
    return scene_->register_object(desc);
}

void PictorRenderer::unregister_object(ObjectId id) {
    if (!initialized_) return;
    scene_->unregister_object(id);
}

void PictorRenderer::update_transform(ObjectId id, const float4x4& transform) {
    if (!initialized_) return;
    scene_->update_transform(id, transform);
}

// ---- Compute Update ----

void PictorRenderer::set_compute_update_shader(ShaderHandle shader) {
    if (!initialized_) return;
    scene_->set_compute_update_shader(shader);
}

// ---- Profile Operations ----

bool PictorRenderer::set_profile(const std::string& name) {
    if (!initialized_) return false;

    if (!profile_manager_->set_profile(name)) return false;

    apply_profile(profile_manager_->current_profile());
    return true;
}

void PictorRenderer::register_custom_profile(const PipelineProfileDef& def) {
    if (!initialized_) return;
    profile_manager_->register_profile(def);
}

const std::string& PictorRenderer::current_profile_name() const {
    return profile_manager_->current_profile_name();
}

void PictorRenderer::apply_profile(const PipelineProfileDef& profile) {
    // §8.4: Profile switch procedure
    // 1. Validate new profile (done by ProfileManager)

    // 2. Frame Allocator size change — requires memory reallocation
    //    (simplified: would recreate MemorySubsystem in production)

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
                config_.screen_width, config_.screen_height);
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

// ---- Profiler ----

void PictorRenderer::set_profiler_enabled(bool enabled) {
    if (profiler_) profiler_->set_enabled(enabled);
}

void PictorRenderer::set_overlay_mode(OverlayMode mode) {
    if (profiler_) profiler_->set_overlay_mode(mode);
}

const FrameStats& PictorRenderer::get_frame_stats() const {
    return profiler_->get_frame_stats();
}

// ---- Stats Overlay ----

void PictorRenderer::toggle_stats_overlay() {
    if (stats_overlay_) stats_overlay_->toggle();
}

void PictorRenderer::set_stats_overlay_visible(bool visible) {
    if (stats_overlay_) stats_overlay_->set_visible(visible);
}

bool PictorRenderer::is_stats_overlay_visible() const {
    return stats_overlay_ && stats_overlay_->is_visible();
}

SceneSummary PictorRenderer::get_scene_summary() const {
    SceneSummary s;
    const auto& fs = profiler_->get_frame_stats();
    s.batch_count     = fs.batch_count;
    s.polygon_count   = fs.triangle_count;
    s.draw_call_count = fs.draw_call_count;

    // GI system state
    const auto& profile = profile_manager_->current_profile();
    s.light_enabled = gi_system_ != nullptr;
    s.gi_enabled    = profile.gi_config.gi_probes_enabled && gi_system_ != nullptr;

    s.shadow_enabled     = profile.gi_config.shadow_enabled && gi_system_ != nullptr;
    s.shadow_filter_mode = profile.gi_config.shadow.filter_mode;
    s.shadow_cascades    = profile.gi_config.shadow.cascade_count;
    s.shadow_resolution  = profile.gi_config.shadow.resolution;

    return s;
}

// ---- Extension Points ----

void PictorRenderer::set_update_callback(IUpdateCallback* callback) {
    if (update_scheduler_) update_scheduler_->set_update_callback(callback);
}

void PictorRenderer::set_culling_provider(ICullingProvider* provider) {
    if (culling_) culling_->set_culling_provider(provider);
}

void PictorRenderer::set_batch_policy(IBatchPolicy* policy) {
    if (batch_builder_) batch_builder_->set_batch_policy(policy);
}

void PictorRenderer::set_job_dispatcher(IJobDispatcher* dispatcher) {
    if (update_scheduler_) update_scheduler_->set_job_dispatcher(dispatcher);
}

void PictorRenderer::register_custom_pass(ICustomRenderPass* pass) {
    if (pass_scheduler_) pass_scheduler_->register_custom_pass(pass);
}

// ---- Data Handler ----

TextureHandle PictorRenderer::register_texture(const TextureDescriptor& desc) {
    if (!initialized_) return INVALID_TEXTURE;
    return data_handler_->register_texture(desc);
}

void PictorRenderer::unregister_texture(TextureHandle handle) {
    if (!initialized_) return;
    data_handler_->unregister_texture(handle);
}

MeshHandle PictorRenderer::register_mesh_data(const MeshDataDescriptor& desc) {
    if (!initialized_) return INVALID_MESH;
    return data_handler_->register_mesh(desc);
}

void PictorRenderer::unregister_mesh_data(MeshHandle handle) {
    if (!initialized_) return;
    data_handler_->unregister_mesh(handle);
}

ModelHandle PictorRenderer::register_model(const ModelDescriptor& desc) {
    if (!initialized_) return INVALID_MODEL;
    return data_handler_->register_model(desc);
}

void PictorRenderer::unregister_model(ModelHandle handle) {
    if (!initialized_) return;
    data_handler_->unregister_model(handle);
}

// ---- GI Lighting ----

void PictorRenderer::set_directional_light(const DirectionalLight& light) {
    if (gi_system_) gi_system_->set_directional_light(light);
}

void PictorRenderer::upload_gi_probe_data(const float* sh_data, uint32_t probe_count) {
    if (gi_system_) gi_system_->upload_probe_data(sh_data, probe_count);
}

void PictorRenderer::set_gi_config(const GIConfig& config) {
    if (gi_system_) gi_system_->set_config(config);
}

// ---- GI Bake ----

GIBakeResult PictorRenderer::bake_static_gi() {
    if (!bake_system_) return GIBakeResult{};
    auto result = bake_system_->bake();
    result.bake_timestamp = frame_number_;
    return result;
}

GIBakeResult PictorRenderer::bake_static_gi(BakeProgressCallback progress) {
    if (!bake_system_) return GIBakeResult{};
    auto result = bake_system_->bake(std::move(progress));
    result.bake_timestamp = frame_number_;
    return result;
}

void PictorRenderer::apply_bake(const GIBakeResult& result) {
    if (!bake_system_) return;
    bake_system_->apply(result);

    // Notify GI system how many static objects have baked data
    if (gi_system_) {
        gi_system_->set_baked_static_count(
            static_cast<uint32_t>(result.object_ids.size()));
    }
}

void PictorRenderer::invalidate_bake() {
    if (bake_system_) bake_system_->invalidate();
    if (gi_system_) gi_system_->set_baked_static_count(0);
}

bool PictorRenderer::save_bake(const std::string& path, const GIBakeResult& result) {
    if (!bake_system_) return false;
    return bake_system_->save(path, result);
}

GIBakeResult PictorRenderer::load_bake(const std::string& path) {
    if (!bake_system_) return GIBakeResult{};
    return bake_system_->load(path);
}

void PictorRenderer::set_bake_data_provider(IBakeDataProvider* provider) {
    if (bake_system_) bake_system_->set_bake_data_provider(provider);
}

// ---- Post-Process ----

void PictorRenderer::set_postprocess_config(const PostProcessConfig& config) {
    if (postprocess_) postprocess_->set_config(config);
}

// ---- Data Export ----

void PictorRenderer::begin_profiler_recording(const std::string& path) {
    if (data_exporter_) data_exporter_->begin_recording(path);
}

void PictorRenderer::end_profiler_recording() {
    if (data_exporter_) data_exporter_->end_recording();
}

bool PictorRenderer::export_profiler_json(const std::string& path) {
    return data_exporter_ ? data_exporter_->export_json(path) : false;
}

bool PictorRenderer::export_profiler_chrome_tracing(const std::string& path) {
    return data_exporter_ ? data_exporter_->export_chrome_tracing(path) : false;
}

bool PictorRenderer::export_profiler_csv(const std::string& path) {
    return data_exporter_ ? data_exporter_->export_csv(path) : false;
}

} // namespace pictor
