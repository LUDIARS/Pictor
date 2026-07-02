#include "pictor/core/pictor_renderer.h"
#include "pictor/pipeline/pipeline_profile_serializer.h"

namespace pictor {

PictorRenderer::PictorRenderer() = default;
PictorRenderer::~PictorRenderer() {
    if (initialized_) shutdown();
}

void PictorRenderer::initialize() { initialize(RendererConfig{}); }

void PictorRenderer::initialize(const RendererConfig& config) {
    config_ = config;

    // §12: サブシステムの構築 (所有 + 依存順序) は manager に委譲 (D-1)。
    subsystems_.initialize(config);
    sync_subsystem_aliases_();

    // モバイル lifecycle コントローラ。 レンダラ操作は Hooks 経由に限定する (D-1)。
    MobileLifecycleController::Hooks hooks;
    hooks.current_frame         = [this] { return frame_number_; };
    hooks.flush_frame_allocator = [this] {
        if (memory_) { memory_->begin_frame(); memory_->end_frame(); }
    };
    hooks.active_profile        = [this] { return current_profile_name(); };
    hooks.switch_profile        = [this](const std::string& name) { set_profile(name); };
    mobile_ = std::make_unique<MobileLifecycleController>(
        std::move(hooks), config.mobile_downgrade);

    // GI / bake 委譲ファサード (subsystems_ 越しに live 参照)。
    gi_facade_ = std::make_unique<GIFacade>(subsystems_, frame_number_);

    // Post-Process は host-driven の `PostProcessPipeline` を使う
    // (KuzuSurvivors WorldRenderer 参照)。 PictorRenderer は関与しない。

    initialized_ = true;
}

void PictorRenderer::shutdown() {
    if (!initialized_) return;

    // §12: Release all resources, GPU sync
#ifdef PICTOR_HAS_VULKAN
    // compiled graph の descriptor pool を先に返す (VkDevice が生きている
    // うちに解放する契約 — host は renderer.shutdown() を device 破棄前に呼ぶ)。
    release_render_graph();
#endif
    gi_facade_.reset();
    mobile_.reset();
    subsystems_.shutdown();
    sync_subsystem_aliases_(); // alias をすべて null 化

    initialized_ = false;
}

void PictorRenderer::sync_subsystem_aliases_() {
    memory_             = subsystems_.memory();
    scene_              = subsystems_.scene();
    update_scheduler_   = subsystems_.update_scheduler();
    batch_builder_      = subsystems_.batch_builder();
    culling_            = subsystems_.culling();
    gpu_buffer_manager_ = subsystems_.gpu_buffer_manager();
    gpu_pipeline_       = subsystems_.gpu_pipeline();
    profile_manager_    = subsystems_.profile_manager();
    pass_scheduler_     = subsystems_.pass_scheduler();
    profiler_           = subsystems_.profiler();
    overlay_            = subsystems_.overlay();
    stats_overlay_      = subsystems_.stats_overlay();
    data_exporter_      = subsystems_.data_exporter();
    data_handler_       = subsystems_.data_handler();
    gi_system_          = subsystems_.gi_system();
    bake_system_        = subsystems_.bake_system();
    animation_system_   = subsystems_.animation_system();
}

bool PictorRenderer::is_frame_work_suppressed_() const {
    // ACTIVE 以外の lifecycle 状態では GPU submit を抑制する (scene/handle/GPU
    // resource は温存)。 詳細は MobileLifecycleController。
    return mobile_ && mobile_->frame_work_suppressed();
}

void PictorRenderer::begin_frame(float delta_time) {
    if (!initialized_) return;
    if (is_frame_work_suppressed_()) return;

    delta_time_ = delta_time;
    ++frame_number_;

    // §12, §11.3: Fence wait + Frame Allocator reset + Profiler frame start
    memory_->begin_frame();
    gpu_buffer_manager_->reset_frame_buffers();

    profiler_->begin_frame();
}

void PictorRenderer::render(const Camera& camera) {
    if (!initialized_) return;
    if (is_frame_work_suppressed_()) return;

    auto& frame_alloc = memory_->frame_allocator();

    // §11.3 Step 2a: Animation Update
    profiler_->begin_cpu_section(CpuSection::AnimationUpdate);
    if (animation_system_) {
        animation_system_->update(delta_time_);
    }
    profiler_->end_cpu_section(CpuSection::AnimationUpdate);

    // §11.3 Step 2b: Data Update
    profiler_->begin_cpu_section(CpuSection::DataUpdate);
    update_scheduler_->update(delta_time_);
    profiler_->end_cpu_section(CpuSection::DataUpdate);

    // §11.3 Step 3: Culling
    profiler_->begin_cpu_section(CpuSection::Culling);
    culling_->cull(camera.frustum, frame_alloc);
    profiler_->end_cpu_section(CpuSection::Culling);

    // Record culling stats
    auto cull_stats = culling_->get_stats();
    profiler_->record_visible(cull_stats.visible_objects, cull_stats.culled_objects);

    // §11.3 Step 4: Sort + Step 5: Batch Build
    profiler_->begin_cpu_section(CpuSection::Sort);
    profiler_->begin_cpu_section(CpuSection::BatchBuild);
    batch_builder_->build(frame_alloc);
    profiler_->end_cpu_section(CpuSection::BatchBuild);
    profiler_->end_cpu_section(CpuSection::Sort);

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

    // §11.3 Step 5-6: managed パス巡回 (custom pass のみ実行)。 built-in pass
    // の実描画は host が render_compiled() (→ execute_compiled) で記録する。
    profiler_->begin_cpu_section(CpuSection::CommandEncode);
    pass_scheduler_->execute(*batch_builder_, *culling_, gpu_pipeline_);
    profiler_->end_cpu_section(CpuSection::CommandEncode);

    // draw call / triangle 統計は render_compiled() の recorder オーバーロード
    // が実測値 (発行した vkCmdDrawIndexed 数) を record_draw_calls /
    // record_triangles で集計する (D-2 虚偽統計の是正)。 host が recorder を
    // 使わない場合は 0 のまま = 「未計測」であり、 偽値は入らない。

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

    // Post-process は host-driven (`PostProcessPipeline`)。 managed レンダラは
    // 関与しない — ホストがシーンを HDR ターゲットへ描き自前で record する。

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
    if (is_frame_work_suppressed_()) return;

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

bool PictorRenderer::load_profile_from_file(const std::string& path,
                                            std::string*       error) {
    if (!initialized_) {
        if (error) *error = "renderer not initialized";
        return false;
    }

    // Seed from the currently active profile so a partial config file only
    // needs to express overrides.
    PipelineProfileDef def;
    if (!load_pipeline_profile_file(path, profile_manager_->current_profile(),
                                    def, error)) {
        return false;
    }
    if (def.profile_name.empty()) {
        if (error) *error = "profile JSON has empty profile_name";
        return false;
    }

    // Register (replaces same-named profile) then activate + reconfigure.
    profile_manager_->register_profile(def);
    if (!profile_manager_->set_profile(def.profile_name)) {
        if (error) *error = "failed to activate loaded profile";
        return false;
    }
    apply_profile(profile_manager_->current_profile());
    return true;
}

void PictorRenderer::reload_active_profile() {
    if (!initialized_) return;
    // §8.4 step 5: re-run the profile switch procedure (scheduler reconfigure,
    // batch invalidation, GI/GPU-driven reconfigure) for the current profile.
    apply_profile(profile_manager_->current_profile());
}

const std::string& PictorRenderer::current_profile_name() const {
    return profile_manager_->current_profile_name();
}

// ---- Mobile Lifecycle (MobileLifecycleController へ委譲、 D-1) ----

void PictorRenderer::on_pause()            { if (mobile_) mobile_->on_pause(); }
void PictorRenderer::on_resume()           { if (mobile_) mobile_->on_resume(); }
void PictorRenderer::on_suspend()          { if (mobile_) mobile_->on_suspend(); }
void PictorRenderer::on_surface_lost()     { if (mobile_) mobile_->on_surface_lost(); }
void PictorRenderer::on_surface_regained() { if (mobile_) mobile_->on_surface_regained(); }

void PictorRenderer::on_low_memory(MemoryPressure level) {
    if (mobile_) mobile_->on_low_memory(level);
}

void PictorRenderer::on_thermal_state(ThermalState state) {
    if (mobile_) mobile_->on_thermal_state(state);
}

MobileLifecycleSnapshot PictorRenderer::lifecycle_snapshot() const {
    return mobile_ ? mobile_->snapshot() : MobileLifecycleSnapshot{};
}

void PictorRenderer::set_lifecycle_observer(IMobileLifecycleObserver* observer) {
    if (mobile_) mobile_->set_observer(observer);
}

void PictorRenderer::set_mobile_downgrade_policy(const MobileAutoDowngradePolicy& policy) {
    config_.mobile_downgrade = policy;
    if (mobile_) mobile_->set_policy(policy);
}

void PictorRenderer::apply_profile(const PipelineProfileDef& profile) {
    // §8.4: 下流サブシステムの再構成は manager に委譲 (D-1)。
    subsystems_.apply_profile(profile);
    // gpu_pipeline_ / gi_system_ が再生成・破棄され得るため alias を取り直す。
    sync_subsystem_aliases_();

#ifdef PICTOR_HAS_VULKAN
    // §3.5: compile はプロファイル切替時のみ再実行 (毎フレーム不可)。
    // apply_profile はプロファイル切替 / reload の時だけ呼ばれるので、
    // engaged なら新しい pass 構成で graph を差し替える。
    if (compiled_driver_.engaged() && pass_scheduler_) {
        compiled_driver_.recompile(profile, *pass_scheduler_);
    }
#endif
}

// ---- Compiled Render Graph (系統B 配線, §3.5 / D-2) ----

#ifdef PICTOR_HAS_VULKAN

bool PictorRenderer::compile_render_graph(VkDevice                   device,
                                          const AttachmentRegistry&  attachments,
                                          const RenderPassRegistry&  render_passes,
                                          const FramebufferRegistry& framebuffers,
                                          uint32_t                   flight_count,
                                          uint32_t                   swapchain_image_count)
{
    if (!initialized_ || !pass_scheduler_) return false;

    CompiledPathDriver::Deps deps;
    deps.device                = device;
    deps.attachments           = &attachments;
    deps.render_passes         = &render_passes;
    deps.framebuffers          = &framebuffers;
    deps.flight_count          = flight_count;
    deps.swapchain_image_count = swapchain_image_count;

    if (compiled_driver_.engaged()) {
        // 再呼び出し (swapchain resize 等) — 旧 graph を解放してから
        // 新しい依存で組み直す。
        compiled_driver_.disengage(*pass_scheduler_);
    }
    return compiled_driver_.engage(deps, profile_manager_->current_profile(),
                                   *pass_scheduler_);
}

void PictorRenderer::release_render_graph() {
    if (pass_scheduler_) compiled_driver_.disengage(*pass_scheduler_);
}

bool PictorRenderer::has_compiled_render_graph() const {
    return pass_scheduler_ && pass_scheduler_->has_compiled_graph();
}

void PictorRenderer::render_compiled(VkCommandBuffer cmd, uint32_t flight_index,
                                     uint32_t image_index,
                                     const RenderPassScheduler::PassRecordFn& record)
{
    if (!initialized_ || !pass_scheduler_) return;
    if (is_frame_work_suppressed_()) return;

    pass_scheduler_->execute_compiled(cmd, flight_index, image_index, record);
}

void PictorRenderer::render_compiled(VkCommandBuffer cmd, uint32_t flight_index,
                                     uint32_t image_index,
                                     CompiledBatchRecorder& recorder)
{
    if (!initialized_ || !pass_scheduler_) return;
    if (is_frame_work_suppressed_()) return;

    // render() が組んだ当該フレームのバッチを recorder に渡して記録する。
    recorder.begin_frame(&batch_builder_->batches());
    pass_scheduler_->execute_compiled(
        cmd, flight_index, image_index,
        [&recorder](VkCommandBuffer c, const CompiledPass& cp,
                    uint32_t flight, uint32_t image) {
            recorder.record(c, cp, flight, image);
        });

    // 実測統計を profiler へ (D-2: 虚偽値ではなく発行済みコマンドの集計)。
    const auto& s = recorder.stats();
    profiler_->record_draw_calls(s.draw_calls);
    profiler_->record_triangles(s.triangles);
}

#endif // PICTOR_HAS_VULKAN

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

// ---- GI Lighting / Bake (GIFacade へ委譲、 D-1) ----

void PictorRenderer::set_directional_light(const DirectionalLight& light) {
    if (gi_facade_) gi_facade_->set_directional_light(light);
}

void PictorRenderer::upload_gi_probe_data(const float* sh_data, uint32_t probe_count) {
    if (gi_facade_) gi_facade_->upload_gi_probe_data(sh_data, probe_count);
}

void PictorRenderer::set_gi_config(const GIConfig& config) {
    if (gi_facade_) gi_facade_->set_gi_config(config);
}

GIBakeResult PictorRenderer::bake_static_gi() {
    return gi_facade_ ? gi_facade_->bake_static_gi() : GIBakeResult{};
}

GIBakeResult PictorRenderer::bake_static_gi(BakeProgressCallback progress) {
    return gi_facade_ ? gi_facade_->bake_static_gi(std::move(progress)) : GIBakeResult{};
}

void PictorRenderer::apply_bake(const GIBakeResult& result) {
    if (gi_facade_) gi_facade_->apply_bake(result);
}

void PictorRenderer::invalidate_bake() {
    if (gi_facade_) gi_facade_->invalidate_bake();
}

bool PictorRenderer::save_bake(const std::string& path, const GIBakeResult& result) {
    return gi_facade_ ? gi_facade_->save_bake(path, result) : false;
}

GIBakeResult PictorRenderer::load_bake(const std::string& path) {
    return gi_facade_ ? gi_facade_->load_bake(path) : GIBakeResult{};
}

void PictorRenderer::set_bake_data_provider(IBakeDataProvider* provider) {
    if (gi_facade_) gi_facade_->set_bake_data_provider(provider);
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
