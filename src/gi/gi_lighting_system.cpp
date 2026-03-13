#include "pictor/gi/gi_lighting_system.h"
#include <cmath>
#include <algorithm>

namespace pictor {

GILightingSystem::GILightingSystem(GPUBufferManager& buffer_manager,
                                   SceneRegistry& registry)
    : GILightingSystem(buffer_manager, registry, GIConfig{})
{}

GILightingSystem::GILightingSystem(GPUBufferManager& buffer_manager,
                                   SceneRegistry& registry,
                                   const GIConfig& config)
    : buffer_manager_(buffer_manager)
    , registry_(registry)
    , config_(config)
{
}

GILightingSystem::~GILightingSystem() {
    // GPU resources freed by buffer_manager_ lifetime
}

void GILightingSystem::initialize(uint32_t max_objects,
                                  uint32_t screen_width,
                                  uint32_t screen_height) {
    max_objects_ = max_objects;
    screen_width_ = screen_width;
    screen_height_ = screen_height;

    // Allocate shadow resources
    if (config_.shadow_enabled) {
        // Per-object cascade membership flags
        resources_.shadow_cascade_flags =
            buffer_manager_.allocate_instance_data(max_objects * sizeof(uint32_t));

        // Per-object × 4 cascade depth values
        resources_.shadow_depths =
            buffer_manager_.allocate_instance_data(max_objects * 4 * sizeof(float));
    }

    // Allocate SSAO resources
    if (config_.ssao_enabled) {
        // Hemisphere sample kernel (vec4 × sample_count)
        resources_.ssao_kernel =
            buffer_manager_.allocate_instance_data(
                config_.ssao.sample_count * 4 * sizeof(float));

        // AO output image (R8, screen resolution)
        resources_.ao_output =
            buffer_manager_.allocate_instance_data(screen_width * screen_height);
    }

    // Allocate GI probe resources
    if (config_.gi_probes_enabled) {
        uint32_t total_probes = config_.probes.grid_x
                              * config_.probes.grid_y
                              * config_.probes.grid_z;

        // Probe SH data: 9 × vec4 per probe
        resources_.probe_irradiance =
            buffer_manager_.allocate_instance_data(total_probes * 9 * 4 * sizeof(float));

        // Per-object interpolated irradiance: 9 × vec4 per object
        resources_.object_irradiance =
            buffer_manager_.allocate_instance_data(max_objects * 9 * 4 * sizeof(float));
    }

    initialized_ = true;
}

void GILightingSystem::execute(const float4x4& camera_view,
                               const float4x4& camera_projection) {
    if (!initialized_) return;

    uint32_t object_count = registry_.total_object_count();
    if (object_count == 0) return;

    stats_ = {};

    // Pass 1: Shadow map generation (per-object cascade assignment + depth)
    if (config_.shadow_enabled) {
        execute_shadow_pass(camera_view, camera_projection);
    }

    // Pass 2: Screen-space ambient occlusion
    if (config_.ssao_enabled) {
        execute_ssao_pass(camera_projection);
    }

    // Pass 3: GI probe sampling (per-object irradiance interpolation)
    if (config_.gi_probes_enabled) {
        execute_gi_probe_pass();
    }
}

void GILightingSystem::set_directional_light(const DirectionalLight& light) {
    directional_light_ = light;
}

void GILightingSystem::upload_probe_data(const float* sh_data, uint32_t probe_count) {
    if (!initialized_ || !config_.gi_probes_enabled) return;

    // In a real Vulkan implementation:
    // 1. Allocate staging buffer
    // 2. memcpy sh_data → staging
    // 3. vkCmdCopyBuffer staging → probe_irradiance SSBO
    // 4. Insert memory barrier
    (void)sh_data;

    stats_.active_probes = probe_count;
}

void GILightingSystem::set_config(const GIConfig& config) {
    config_ = config;
}

// ============================================================
// Shadow Map Pass
// ============================================================

void GILightingSystem::execute_shadow_pass(const float4x4& camera_view,
                                           const float4x4& camera_projection) {
    uint32_t object_count = registry_.total_object_count();
    uint32_t cascade_count = config_.shadow.cascade_count;
    cascade_count = std::min(cascade_count, 4u);

    // Compute cascade split distances using practical-logarithmic scheme
    float splits[5]; // near + cascade_count far planes
    compute_cascade_splits(0.1f, config_.shadow.max_shadow_dist,
                           splits, cascade_count);

    // For each cascade, compute the light-space view-projection
    // These matrices are uploaded as a uniform buffer for the compute shader
    // In a real Vulkan implementation:
    //   1. Update ShadowParams uniform buffer with lightViewProj[cascade_count]
    //   2. Bind gpu_bounds, gpu_transforms, gpu_visibility as input SSBOs
    //   3. Bind shadow_cascade_flags, shadow_depths as output SSBOs
    //   4. vkCmdDispatch(cmd, workgroups, 1, 1) — shadow_map_gen.comp

    uint32_t workgroups = calculate_workgroups(object_count);

    stats_.shadow_casters = object_count;
    stats_.active_cascades = cascade_count;
    stats_.gi_workgroups += workgroups;
}

// ============================================================
// SSAO Pass
// ============================================================

void GILightingSystem::execute_ssao_pass(const float4x4& camera_projection) {
    // In a real Vulkan implementation:
    //   1. Update SSAOParams uniform buffer (projection, invProjection, etc.)
    //   2. Ensure sample kernel SSBO is initialized
    //   3. Bind depth texture, normal texture, noise texture
    //   4. Bind ao_output as storage image
    //   5. vkCmdDispatch(cmd, ceil(w/16), ceil(h/16), 1) — ssao_gen.comp
    //   6. Optionally dispatch bilateral blur pass on ao_output

    (void)camera_projection;

    uint32_t dispatch_x = (screen_width_  + 15) / 16;
    uint32_t dispatch_y = (screen_height_ + 15) / 16;

    stats_.ssao_samples = config_.ssao.sample_count;
    stats_.gi_workgroups += dispatch_x * dispatch_y;
}

// ============================================================
// GI Probe Sampling Pass
// ============================================================

void GILightingSystem::execute_gi_probe_pass() {
    if (!config_.gi_probes_enabled) return;

    uint32_t object_count = registry_.total_object_count();

    // In a real Vulkan implementation:
    //   1. Update GIProbeParams uniform buffer
    //   2. Bind gpu_transforms, gpu_visibility as input SSBOs
    //   3. Bind probe_irradiance as input SSBO
    //   4. Bind object_irradiance as output SSBO
    //   5. vkCmdDispatch(cmd, workgroups, 1, 1) — gi_probe_sample.comp

    uint32_t workgroups = calculate_workgroups(object_count);

    uint32_t total_probes = config_.probes.grid_x
                          * config_.probes.grid_y
                          * config_.probes.grid_z;
    stats_.active_probes = total_probes;
    stats_.gi_workgroups += workgroups;
}

// ============================================================
// Cascade Split Computation
// ============================================================

void GILightingSystem::compute_cascade_splits(float near_plane, float far_plane,
                                              float splits[], uint32_t count) const {
    // Practical-logarithmic split scheme (GPU Gems 3, Ch. 10)
    // Blends uniform and logarithmic distributions:
    //   C_i = lambda * log_split + (1 - lambda) * uniform_split
    float lambda = config_.shadow.cascade_lambda;

    splits[0] = near_plane;
    for (uint32_t i = 1; i <= count; i++) {
        float fraction = static_cast<float>(i) / static_cast<float>(count);

        float log_split = near_plane * std::pow(far_plane / near_plane, fraction);
        float uniform_split = near_plane + (far_plane - near_plane) * fraction;

        splits[i] = lambda * log_split + (1.0f - lambda) * uniform_split;
    }
}

// ============================================================
// Light-Space View-Projection
// ============================================================

float4x4 GILightingSystem::compute_light_view_proj(
    const float4x4& camera_view,
    const float4x4& camera_projection,
    float near_split, float far_split) const
{
    // Compute light-space view-projection for a single CSM cascade.
    // In a full implementation this would:
    //   1. Compute camera frustum corners at [near_split, far_split]
    //   2. Transform corners to world space
    //   3. Fit an orthographic projection around them from the light direction
    //   4. Apply texel snapping to reduce shimmer
    //
    // For now, return a placeholder that follows the light direction.
    (void)camera_view;
    (void)camera_projection;
    (void)near_split;
    (void)far_split;

    float4x4 lvp = float4x4::identity();

    // Build a simple orthographic view from light direction
    float3 dir = directional_light_.direction;
    float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len > 0.0f) {
        dir.x /= len;
        dir.y /= len;
        dir.z /= len;
    }

    // The actual tight-fit cascade matrix computation is deferred to
    // Vulkan command recording where camera frustum data is available.
    return lvp;
}

uint32_t GILightingSystem::calculate_workgroups(uint32_t count, uint32_t size) const {
    return (count + size - 1) / size;
}

} // namespace pictor
