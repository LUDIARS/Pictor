#pragma once

#include "pictor/core/types.h"
#include "pictor/memory/memory_subsystem.h"
#include "pictor/scene/scene_registry.h"
#include "pictor/update/update_scheduler.h"
#include "pictor/batch/batch_builder.h"
#include "pictor/culling/culling_system.h"
#include "pictor/gpu/gpu_buffer_manager.h"
#include "pictor/gpu/gpu_driven_pipeline.h"
#include "pictor/pipeline/pipeline_profile.h"
#include "pictor/pipeline/render_pass_scheduler.h"
#include "pictor/pipeline/command_encoder.h"
#include "pictor/profiler/profiler.h"
#include "pictor/profiler/overlay_renderer.h"
#include "pictor/profiler/stats_overlay.h"
#include "pictor/profiler/data_exporter.h"
#include "pictor/data/data_handler.h"
#include "pictor/data/data_query_api.h"
#include "pictor/gi/gi_lighting_system.h"
#include "pictor/gi/gi_bake.h"
#include "pictor/postprocess/postprocess_pipeline.h"
#include "pictor/animation/animation_system.h"
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
    GILightingSystem* gi_system() { return gi_system_.get(); }

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
    GIBakeSystem* bake_system() { return bake_system_.get(); }

    // ---- Post-Process Pipeline ----

    /// Access the post-process pipeline
    PostProcessPipeline& post_process() { return *postprocess_; }
    const PostProcessPipeline& post_process() const { return *postprocess_; }

    /// Set full post-process configuration
    void set_postprocess_config(const PostProcessConfig& config);

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
    /// Profile switch procedure (§8.4)
    void apply_profile(const PipelineProfileDef& profile);

    bool initialized_ = false;

    // Subsystems (owned)
    std::unique_ptr<MemorySubsystem>        memory_;
    std::unique_ptr<SceneRegistry>          scene_;
    std::unique_ptr<UpdateScheduler>        update_scheduler_;
    std::unique_ptr<BatchBuilder>           batch_builder_;
    std::unique_ptr<CullingSystem>          culling_;
    std::unique_ptr<GPUBufferManager>       gpu_buffer_manager_;
    std::unique_ptr<GPUDrivenPipeline>      gpu_pipeline_;
    std::unique_ptr<PipelineProfileManager> profile_manager_;
    std::unique_ptr<RenderPassScheduler>    pass_scheduler_;
    std::unique_ptr<CommandEncoder>         command_encoder_;
    std::unique_ptr<Profiler>               profiler_;
    std::unique_ptr<OverlayRenderer>        overlay_;
    std::unique_ptr<StatsOverlay>           stats_overlay_;
    std::unique_ptr<DataExporter>           data_exporter_;
    std::unique_ptr<DataHandler>            data_handler_;
    std::unique_ptr<GILightingSystem>       gi_system_;
    std::unique_ptr<GIBakeSystem>           bake_system_;
    std::unique_ptr<PostProcessPipeline>    postprocess_;
    std::unique_ptr<AnimationSystem>        animation_system_;

    RendererConfig config_;
    float          delta_time_     = 0.0f;
    uint64_t       frame_number_   = 0;
};

} // namespace pictor
