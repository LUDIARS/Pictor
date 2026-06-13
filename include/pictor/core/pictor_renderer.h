#pragma once

#include "pictor/core/types.h"
#include "pictor/core/mobile_lifecycle.h"
#include "pictor/memory/memory_subsystem.h"
#include "pictor/scene/scene_registry.h"
#include "pictor/update/update_scheduler.h"
#include "pictor/batch/batch_builder.h"
#include "pictor/culling/culling_system.h"
#include "pictor/gpu/gpu_buffer_manager.h"
#include "pictor/gpu/gpu_driven_pipeline.h"
#include "pictor/pipeline/pipeline_profile.h"
#include "pictor/pipeline/render_pass_scheduler.h"
#include "pictor/profiler/profiler.h"
#include "pictor/profiler/overlay_renderer.h"
#include "pictor/profiler/stats_overlay.h"
#include "pictor/profiler/data_exporter.h"
#include "pictor/data/data_handler.h"
#include "pictor/data/data_query_api.h"
#include "pictor/gi/gi_lighting_system.h"
#include "pictor/gi/gi_bake.h"
#include "pictor/animation/animation_system.h"
#include "pictor/core/renderer_subsystem_manager.h"
#include "pictor/core/mobile_lifecycle_controller.h"
#include "pictor/core/gi_facade.h"
#include <memory>

namespace pictor {

/// Renderer initialization config
struct RendererConfig {
    std::string       initial_profile = "Standard";
    MemoryConfig      memory_config;
    UpdateConfig      update_config;
    uint32_t          screen_width  = 1920;
    uint32_t          screen_height = 1080;
    bool              profiler_enabled = true;
    OverlayMode       overlay_mode = OverlayMode::STANDARD;

    /// Mobile auto-downgrade policy. Off by default — desktop
    /// hosts leave `enabled = false`. Android / iOS hosts that
    /// want Pictor to swap to `low_profile_name` on thermal
    /// throttling flip `enabled = true`.
    MobileAutoDowngradePolicy mobile_downgrade;
};

/// Camera for rendering
struct Camera {
    float4x4 view       = float4x4::identity();
    float4x4 projection = float4x4::identity();
    float3   position;
    Frustum  frustum;
};

/// Main Pictor renderer — public API entry point (§12).
///
/// Lifecycle:
///   Initialize(config) → [BeginFrame → Render → EndFrame]* → Shutdown
///
/// Manages all subsystems and orchestrates the rendering pipeline.
class PictorRenderer {
public:
    PictorRenderer();
    ~PictorRenderer();

    PictorRenderer(const PictorRenderer&) = delete;
    PictorRenderer& operator=(const PictorRenderer&) = delete;

    // ---- Lifecycle (§12) ----

    /// Initialize backend, memory, profiler, default profile
    void initialize();
    void initialize(const RendererConfig& config);

    /// Shutdown: release all resources, GPU sync
    void shutdown();

    bool is_initialized() const { return initialized_; }

    // ---- Frame Rendering (§12) ----

    /// Begin frame: fence wait + Frame Allocator reset + Profiler frame start
    void begin_frame(float delta_time);

    /// Render with given camera and scene
    void render(const Camera& camera);

    /// End frame: present
    void end_frame();

    // ---- Object Operations (§12) ----

    ObjectId register_object(const ObjectDescriptor& desc);
    void     unregister_object(ObjectId id);
    void     update_transform(ObjectId id, const float4x4& transform);

    // ---- Compute Update (§12) ----

    void set_compute_update_shader(ShaderHandle shader);

    // ---- Profile Operations (§12) ----

    bool set_profile(const std::string& name);
    void register_custom_profile(const PipelineProfileDef& def);
    const std::string& current_profile_name() const;

    /// Load a pipeline profile from an external JSON file (系統A external
    /// config, see pipeline_profile_serializer.h), register it, and make it
    /// the active profile — re-running apply_profile() so the RenderPassScheduler,
    /// GI system, GPU-driven pipeline and update scheduler all reconfigure.
    /// This is the runtime-reload seam for tooling (e.g. the Ergo-web editor).
    /// Returns false on I/O or parse error; `error` (optional) carries why.
    bool load_profile_from_file(const std::string& path,
                                std::string*       error = nullptr);

    /// Re-apply the currently active profile after it has been mutated in
    /// place (e.g. re-registered via register_custom_profile with the same
    /// name). Reconfigures all downstream subsystems. No-op if uninitialized.
    void reload_active_profile();

    // ---- Mobile Lifecycle ----
    //
    // Platform hosts (Android JNI / iOS) forward OS-level
    // lifecycle events here. `begin_frame` / `render` / `end_frame`
    // become no-ops while `lifecycle()` is not `ACTIVE`; the
    // scene graph, handles, and GPU resources are otherwise
    // preserved so resume is cheap.

    void on_pause();             // → LifecycleState::PAUSED
    void on_resume();            // → LifecycleState::ACTIVE
    void on_suspend();           // → LifecycleState::SUSPENDED (stricter than pause)
    void on_surface_lost();      // swap-chain invalid; host will recreate
    void on_surface_regained();  // clears SURFACE_LOST; state restored
    void on_low_memory(MemoryPressure level);
    void on_thermal_state(ThermalState state);

    MobileLifecycleSnapshot lifecycle_snapshot() const;

    /// Install an observer that receives lifecycle transitions.
    /// Ownership stays with the caller; pass nullptr to detach.
    void set_lifecycle_observer(IMobileLifecycleObserver* observer);

    /// Configure / mutate the auto-downgrade policy at runtime.
    void set_mobile_downgrade_policy(const MobileAutoDowngradePolicy& policy);

    // ---- Profiler (§12) ----

    void set_profiler_enabled(bool enabled);
    void set_overlay_mode(OverlayMode mode);
    const FrameStats& get_frame_stats() const;

    // ---- Stats Overlay (S key toggle) ----

    /// Toggle stats overlay visibility
    void toggle_stats_overlay();

    /// Set stats overlay visibility explicitly
    void set_stats_overlay_visible(bool visible);

    /// Check if stats overlay is visible
    bool is_stats_overlay_visible() const;

    /// Build current scene summary for external queries
    SceneSummary get_scene_summary() const;

    // ---- Animation System ----

    /// Access the animation system
    AnimationSystem&       animation()       { return *animation_system_; }
    const AnimationSystem& animation() const { return *animation_system_; }

    // ---- Extension Points (§12.2) ----

    void set_update_callback(IUpdateCallback* callback);
    void set_culling_provider(ICullingProvider* provider);
    void set_batch_policy(IBatchPolicy* policy);
    void set_job_dispatcher(IJobDispatcher* dispatcher);
    void register_custom_pass(ICustomRenderPass* pass);

    // ---- Data Handler ----

    /// Register a texture via the data handler
    TextureHandle register_texture(const TextureDescriptor& desc);
    void unregister_texture(TextureHandle handle);

    /// Register a mesh with flexible vertex data via the data handler
    MeshHandle register_mesh_data(const MeshDataDescriptor& desc);
    void unregister_mesh_data(MeshHandle handle);

    /// Register a 3D model (skin meshes, rig, animations) via the data handler
    ModelHandle register_model(const ModelDescriptor& desc);
    void unregister_model(ModelHandle handle);

    /// Access the data handler directly
    DataHandler&       data_handler()       { return *data_handler_; }
    const DataHandler& data_handler() const { return *data_handler_; }

    /// Create a read-only query API for external tools/editors
    DataQueryAPI create_query_api() const { return DataQueryAPI(*data_handler_); }

    // ---- GI Lighting ----

    /// Set the primary directional light for shadow mapping
    void set_directional_light(const DirectionalLight& light);

    /// Upload light probe irradiance data for GI
    void upload_gi_probe_data(const float* sh_data, uint32_t probe_count);

    /// Access GI system configuration
    void set_gi_config(const GIConfig& config);
    GILightingSystem* gi_system() { return gi_system_; }

    // ---- GI Bake (static objects) ----

    /// Bake GI data for all static-pool objects (blocking)
    GIBakeResult bake_static_gi();

    /// Bake with progress callback (return false to cancel)
    GIBakeResult bake_static_gi(BakeProgressCallback progress);

    /// Apply baked result to GPU for runtime use
    void apply_bake(const GIBakeResult& result);

    /// Mark bake as stale (call after modifying static objects)
    void invalidate_bake();

    /// Save / load bake data
    bool save_bake(const std::string& path, const GIBakeResult& result);
    GIBakeResult load_bake(const std::string& path);

    /// Set a data provider for bake (extra lights, emissive surfaces, etc.)
    void set_bake_data_provider(IBakeDataProvider* provider);

    /// Access bake system
    GIBakeSystem* bake_system() { return bake_system_; }

    // ---- Post-Process ----
    // host-driven の `pictor::PostProcessPipeline` を使う。 ホストがシーンを
    // HDR ターゲットへ描き、 自前で initialize_vulkan / record する
    // (PrivateGame WorldRenderer 参照)。 PictorRenderer は関与しない。

    // ---- Data Export (§13.7) ----

    void begin_profiler_recording(const std::string& path);
    void end_profiler_recording();
    bool export_profiler_json(const std::string& path);
    bool export_profiler_chrome_tracing(const std::string& path);
    bool export_profiler_csv(const std::string& path);

    // ---- Subsystem Access ----

    MemorySubsystem&        memory()          { return *memory_; }
    SceneRegistry&          scene()           { return *scene_; }
    Profiler&               profiler()        { return *profiler_; }
    PipelineProfileManager& profile_manager() { return *profile_manager_; }

private:
    /// Profile switch procedure (§8.4)。 subsystems_.apply_profile に委譲した後、
    /// 動的に生成/破棄される alias (gpu_pipeline_ / gi_system_ / bake_system_) を再同期。
    void apply_profile(const PipelineProfileDef& profile);

    /// subsystems_ が所有する実体から非所有 alias を取り直す。
    void sync_subsystem_aliases_();

    bool is_frame_work_suppressed_() const;

    bool initialized_ = false;

    // サブシステムの所有・構築順序・プロファイル再構成は manager に集約 (D-1)。
    RendererSubsystemManager subsystems_;

    // 非所有 alias (subsystems_ が所有)。 本体 hot path / API 実装の簡潔さのために
    // 保持し、 initialize() / apply_profile() 後に sync_subsystem_aliases_() で更新。
    MemorySubsystem*        memory_             = nullptr;
    SceneRegistry*          scene_              = nullptr;
    UpdateScheduler*        update_scheduler_   = nullptr;
    BatchBuilder*           batch_builder_      = nullptr;
    CullingSystem*          culling_            = nullptr;
    GPUBufferManager*       gpu_buffer_manager_ = nullptr;
    GPUDrivenPipeline*      gpu_pipeline_       = nullptr;
    PipelineProfileManager* profile_manager_    = nullptr;
    RenderPassScheduler*    pass_scheduler_     = nullptr;
    Profiler*               profiler_           = nullptr;
    OverlayRenderer*        overlay_            = nullptr;
    StatsOverlay*           stats_overlay_      = nullptr;
    DataExporter*           data_exporter_      = nullptr;
    DataHandler*            data_handler_       = nullptr;
    GILightingSystem*       gi_system_          = nullptr;
    GIBakeSystem*           bake_system_        = nullptr;
    AnimationSystem*        animation_system_   = nullptr;

    // 切り出したコントローラ / ファサード (initialize で生成)。
    std::unique_ptr<MobileLifecycleController> mobile_;
    std::unique_ptr<GIFacade>                  gi_facade_;

    RendererConfig config_;
    float          delta_time_     = 0.0f;
    uint64_t       frame_number_   = 0;
};

} // namespace pictor
