#pragma once

#include "pictor/core/types.h"
#include "pictor/gpu/gpu_buffer_manager.h"
#include "pictor/scene/scene_registry.h"

namespace pictor {

// ============================================================
// Light types for GI system
// ============================================================

struct DirectionalLight {
    float3 direction = {0.0f, -1.0f, 0.0f};
    float  intensity = 1.0f;
    float3 color     = {1.0f, 1.0f, 1.0f};
    float  padding0  = 0.0f;
};

struct PointLight {
    float3 position  = {};
    float  radius    = 10.0f;
    float3 color     = {1.0f, 1.0f, 1.0f};
    float  intensity = 1.0f;
};

// ============================================================
// GI Configuration
// ============================================================

/// Shadow map generation settings
struct ShadowMapConfig {
    uint32_t         cascade_count     = 3;       // CSM cascade count (1..4)
    uint32_t         resolution        = 2048;    // per-cascade resolution
    float            depth_bias        = 0.005f;  // constant depth bias
    float            normal_bias       = 0.02f;   // normal-based bias
    float            slope_scale_bias  = 0.005f;  // slope-scale depth bias
    float            cascade_lambda    = 0.75f;   // practical-logarithmic split factor
    float            max_shadow_dist   = 200.0f;  // max distance for shadow casting
    float            cascade_blend_width = 0.1f;  // blend width at cascade boundaries
    ShadowFilterMode filter_mode       = ShadowFilterMode::PCF;
    float            shadow_strength   = 1.0f;    // 0 = no shadow, 1 = full shadow

    // PCSS parameters (only used when filter_mode == PCSS)
    float            pcss_light_size        = 0.04f;  // light source size
    float            pcss_min_penumbra      = 0.5f;   // minimum penumbra width (texels)
    float            pcss_max_penumbra      = 16.0f;  // maximum penumbra width (texels)
    float            pcss_blocker_search_radius = 8.0f; // blocker search radius (texels)
};

/// Screen-space AO settings
struct SSAOConfig {
    uint32_t sample_count      = 32;      // hemisphere samples
    float    radius             = 0.5f;   // sampling radius (world-space)
    float    bias               = 0.025f; // angle bias
    float    intensity          = 1.0f;   // AO strength
    float    falloff_start      = 50.0f;  // distance-based falloff start
    float    falloff_end        = 100.0f; // distance-based falloff end
    bool     blur_enabled       = true;   // bilateral blur pass
};

/// Light probe grid settings
struct GIProbeConfig {
    float3   grid_origin   = {0.0f, 0.0f, 0.0f};
    float3   grid_spacing  = {4.0f, 4.0f, 4.0f};
    uint32_t grid_x        = 16;  // probe count per axis
    uint32_t grid_y        = 8;
    uint32_t grid_z        = 16;
    float    gi_intensity   = 1.0f;
    float    max_probe_distance = 16.0f;
};

/// Full GI system configuration
struct GIConfig {
    ShadowMapConfig shadow;
    SSAOConfig      ssao;
    GIProbeConfig   probes;
    bool            shadow_enabled   = true;
    bool            ssao_enabled     = true;
    bool            gi_probes_enabled = false;  // opt-in (requires probe data)
};

// ============================================================
// GPU resource layout for GI
// ============================================================

/// GPU-side shadow uniform data (mirrors ShadowUniforms in shadow.glsl)
struct ShadowUniformData {
    float4x4 shadow_view_proj[4];             // per-cascade light VP matrices
    float    cascade_split_depths[4] = {};    // view-space Z split distances
    float    texel_size       = 0.0f;         // 1.0 / resolution
    float    cascade_count_f  = 3.0f;         // cascade count as float
    float    filter_mode_f    = 1.0f;         // ShadowFilterMode as float
    float    shadow_strength  = 1.0f;         // shadow intensity
    float    depth_bias       = 0.005f;
    float    normal_bias      = 0.02f;
    float    slope_scale_bias = 0.005f;
    float    cascade_blend_width = 0.1f;
    float    pcss_light_size        = 0.04f;
    float    pcss_min_penumbra      = 0.5f;
    float    pcss_max_penumbra      = 16.0f;
    float    pcss_blocker_search_radius = 8.0f;
};

/// GPU allocations managed by the GI system
struct GIResourceLayout {
    // Shadow map resources
    GpuAllocation shadow_cascade_flags;   // uint per object
    GpuAllocation shadow_depths;          // 4 floats per object (per-cascade)
    GpuAllocation shadow_atlas;           // depth texture array (cascadeCount layers)
    GpuAllocation shadow_uniforms;        // ShadowUniformData uniform buffer

    // SSAO resources
    GpuAllocation ssao_kernel;            // vec4 × sample_count
    GpuAllocation ao_output;              // screen-size R8 image

    // GI probe resources
    GpuAllocation probe_irradiance;       // 9 × vec4 per probe (SH L2)
    GpuAllocation object_irradiance;      // 9 × vec4 per object (interpolated)
};

// ============================================================
// GI Lighting System
// ============================================================

/// Global Illumination lighting system.
/// Pre-generates shadow maps and occlusion maps independently of
/// per-material shaders. Outputs bind as read-only textures/SSBOs
/// that fragment shaders can sample without coupling to GI internals.
///
/// Pipeline integration:
///   ShadowMapGen → SSAO → GIProbes → (existing shaders read results)
class GILightingSystem {
public:
    GILightingSystem(GPUBufferManager& buffer_manager,
                     SceneRegistry& registry);
    GILightingSystem(GPUBufferManager& buffer_manager,
                     SceneRegistry& registry,
                     const GIConfig& config);
    ~GILightingSystem();

    GILightingSystem(const GILightingSystem&) = delete;
    GILightingSystem& operator=(const GILightingSystem&) = delete;

    /// Initialize GPU resources (call after GPUDrivenPipeline::initialize)
    void initialize(uint32_t max_objects, uint32_t screen_width, uint32_t screen_height);

    /// Execute GI pre-passes for the current frame.
    /// Call after Compute Cull (visibility data must be ready).
    /// @param camera_view       current camera view matrix
    /// @param camera_projection current camera projection matrix
    void execute(const float4x4& camera_view,
                 const float4x4& camera_projection);

    /// Set/update the primary directional light (for CSM shadows)
    void set_directional_light(const DirectionalLight& light);

    /// Upload/update probe irradiance data (CPU → GPU)
    void upload_probe_data(const float* sh_data, uint32_t probe_count);

    // ---- Configuration ----

    void set_config(const GIConfig& config);
    const GIConfig& config() const { return config_; }

    void set_shadow_config(const ShadowMapConfig& cfg) { config_.shadow = cfg; }
    void set_ssao_config(const SSAOConfig& cfg)        { config_.ssao = cfg; }
    void set_probe_config(const GIProbeConfig& cfg)    { config_.probes = cfg; }

    void set_shadow_enabled(bool enabled)    { config_.shadow_enabled = enabled; }
    void set_ssao_enabled(bool enabled)      { config_.ssao_enabled = enabled; }
    void set_gi_probes_enabled(bool enabled) { config_.gi_probes_enabled = enabled; }

    // ---- Resource access (for shader binding) ----

    const GIResourceLayout& resource_layout() const { return resources_; }

    /// Get current shadow uniform data (for binding to fragment shaders)
    const ShadowUniformData& shadow_uniforms() const { return shadow_uniform_data_; }

    /// Get cascade split distances (view-space)
    const float* cascade_splits() const { return cached_cascade_splits_; }

    /// Get light-space view-projection for a cascade
    const float4x4& cascade_view_proj(uint32_t cascade) const {
        return shadow_uniform_data_.shadow_view_proj[cascade];
    }

    // ---- Bake integration ----

    /// Set the baked static object count. When > 0, runtime GI passes
    /// skip the first `count` static objects and use pre-baked GPU data.
    void set_baked_static_count(uint32_t count) { baked_static_count_ = count; }
    uint32_t baked_static_count() const { return baked_static_count_; }

    // ---- Statistics ----

    struct Stats {
        uint32_t shadow_casters    = 0;
        uint32_t active_cascades   = 0;
        uint32_t ssao_samples      = 0;
        uint32_t active_probes     = 0;
        uint32_t gi_workgroups     = 0;
    };

    Stats get_stats() const { return stats_; }
    bool is_initialized() const { return initialized_; }

private:
    /// Execute shadow map generation pass
    void execute_shadow_pass(const float4x4& camera_view,
                             const float4x4& camera_projection);

    /// Execute SSAO generation pass
    void execute_ssao_pass(const float4x4& camera_projection);

    /// Execute GI probe sampling pass
    void execute_gi_probe_pass();

    /// Compute cascade split distances
    void compute_cascade_splits(float near_plane, float far_plane,
                                float splits[], uint32_t count) const;

    /// Compute light-space view-projection for a cascade
    float4x4 compute_light_view_proj(const float4x4& camera_view,
                                     const float4x4& camera_projection,
                                     float near_split, float far_split) const;

    /// Calculate workgroup count
    uint32_t calculate_workgroups(uint32_t count, uint32_t size = 256) const;

    /// Build shadow uniform data from current config and cascade matrices
    void update_shadow_uniforms(const float4x4& camera_view,
                                const float4x4& camera_projection);

    /// Render shadow depth pass for a single cascade
    void render_shadow_cascade(uint32_t cascade_index,
                               const float4x4& light_view_proj);

    GPUBufferManager&  buffer_manager_;
    SceneRegistry&     registry_;
    GIConfig           config_;
    GIResourceLayout   resources_;
    DirectionalLight   directional_light_;
    ShadowUniformData  shadow_uniform_data_;
    float              cached_cascade_splits_[5] = {};
    Stats              stats_;
    bool               initialized_ = false;
    uint32_t           max_objects_  = 0;
    uint32_t           screen_width_ = 0;
    uint32_t           screen_height_ = 0;
    uint32_t           baked_static_count_ = 0;
};

} // namespace pictor
