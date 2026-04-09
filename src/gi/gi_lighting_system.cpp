#include "pictor/gi/gi_lighting_system.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace pictor {

// ============================================================
// Math helpers (local to this translation unit)
// ============================================================

namespace {

float dot3(const float3& a, const float3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float length3(const float3& v) {
    return std::sqrt(dot3(v, v));
}

float3 normalize3(const float3& v) {
    float len = length3(v);
    if (len < 1e-8f) return {0.0f, 0.0f, 0.0f};
    return v * (1.0f / len);
}

float3 cross3(const float3& a, const float3& b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

float4x4 multiply4x4(const float4x4& a, const float4x4& b) {
    float4x4 r{};
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            r.m[i][j] = 0.0f;
            for (int k = 0; k < 4; k++) {
                r.m[i][j] += a.m[i][k] * b.m[k][j];
            }
        }
    }
    return r;
}

/// Invert a 4x4 matrix (general case).
/// Returns identity if matrix is singular.
float4x4 inverse4x4(const float4x4& m) {
    float4x4 inv;
    float det;
    float* o = &inv.m[0][0];
    const float* i = &m.m[0][0];

    o[0]  =  i[5]*i[10]*i[15] - i[5]*i[11]*i[14] - i[9]*i[6]*i[15]
           + i[9]*i[7]*i[14]  + i[13]*i[6]*i[11]  - i[13]*i[7]*i[10];
    o[4]  = -i[4]*i[10]*i[15] + i[4]*i[11]*i[14]  + i[8]*i[6]*i[15]
           - i[8]*i[7]*i[14]  - i[12]*i[6]*i[11]  + i[12]*i[7]*i[10];
    o[8]  =  i[4]*i[9]*i[15]  - i[4]*i[11]*i[13]  - i[8]*i[5]*i[15]
           + i[8]*i[7]*i[13]  + i[12]*i[5]*i[11]  - i[12]*i[7]*i[9];
    o[12] = -i[4]*i[9]*i[14]  + i[4]*i[10]*i[13]  + i[8]*i[5]*i[14]
           - i[8]*i[6]*i[13]  - i[12]*i[5]*i[10]  + i[12]*i[6]*i[9];
    o[1]  = -i[1]*i[10]*i[15] + i[1]*i[11]*i[14]  + i[9]*i[2]*i[15]
           - i[9]*i[3]*i[14]  - i[13]*i[2]*i[11]  + i[13]*i[3]*i[10];
    o[5]  =  i[0]*i[10]*i[15] - i[0]*i[11]*i[14]  - i[8]*i[2]*i[15]
           + i[8]*i[3]*i[14]  + i[12]*i[2]*i[11]  - i[12]*i[3]*i[10];
    o[9]  = -i[0]*i[9]*i[15]  + i[0]*i[11]*i[13]  + i[8]*i[1]*i[15]
           - i[8]*i[3]*i[13]  - i[12]*i[1]*i[11]  + i[12]*i[3]*i[9];
    o[13] =  i[0]*i[9]*i[14]  - i[0]*i[10]*i[13]  - i[8]*i[1]*i[14]
           + i[8]*i[2]*i[13]  + i[12]*i[1]*i[10]  - i[12]*i[2]*i[9];
    o[2]  =  i[1]*i[6]*i[15]  - i[1]*i[7]*i[14]   - i[5]*i[2]*i[15]
           + i[5]*i[3]*i[14]  + i[13]*i[2]*i[7]   - i[13]*i[3]*i[6];
    o[6]  = -i[0]*i[6]*i[15]  + i[0]*i[7]*i[14]   + i[4]*i[2]*i[15]
           - i[4]*i[3]*i[14]  - i[12]*i[2]*i[7]   + i[12]*i[3]*i[6];
    o[10] =  i[0]*i[5]*i[15]  - i[0]*i[7]*i[13]   - i[4]*i[1]*i[15]
           + i[4]*i[3]*i[13]  + i[12]*i[1]*i[7]   - i[12]*i[3]*i[5];
    o[14] = -i[0]*i[5]*i[14]  + i[0]*i[6]*i[13]   + i[4]*i[1]*i[14]
           - i[4]*i[2]*i[13]  - i[12]*i[1]*i[6]   + i[12]*i[2]*i[5];
    o[3]  = -i[1]*i[6]*i[11]  + i[1]*i[7]*i[10]   + i[5]*i[2]*i[11]
           - i[5]*i[3]*i[10]  - i[9]*i[2]*i[7]    + i[9]*i[3]*i[6];
    o[7]  =  i[0]*i[6]*i[11]  - i[0]*i[7]*i[10]   - i[4]*i[2]*i[11]
           + i[4]*i[3]*i[10]  + i[8]*i[2]*i[7]    - i[8]*i[3]*i[6];
    o[11] = -i[0]*i[5]*i[11]  + i[0]*i[7]*i[9]    + i[4]*i[1]*i[11]
           - i[4]*i[3]*i[9]   - i[8]*i[1]*i[7]    + i[8]*i[3]*i[5];
    o[15] =  i[0]*i[5]*i[10]  - i[0]*i[6]*i[9]    - i[4]*i[1]*i[10]
           + i[4]*i[2]*i[9]   + i[8]*i[1]*i[6]    - i[8]*i[2]*i[5];

    det = i[0]*o[0] + i[1]*o[4] + i[2]*o[8] + i[3]*o[12];
    if (std::abs(det) < 1e-12f) return float4x4::identity();

    float inv_det = 1.0f / det;
    for (int j = 0; j < 16; j++) o[j] *= inv_det;
    return inv;
}

/// Build a look-at view matrix (right-handed)
float4x4 look_at(const float3& eye, const float3& target, const float3& up) {
    float3 f = normalize3(target - eye);
    float3 r = normalize3(cross3(f, up));
    float3 u = cross3(r, f);

    float4x4 m = float4x4::identity();
    m.m[0][0] =  r.x;  m.m[0][1] =  u.x;  m.m[0][2] = -f.x;
    m.m[1][0] =  r.y;  m.m[1][1] =  u.y;  m.m[1][2] = -f.y;
    m.m[2][0] =  r.z;  m.m[2][1] =  u.z;  m.m[2][2] = -f.z;
    m.m[3][0] = -dot3(r, eye);
    m.m[3][1] = -dot3(u, eye);
    m.m[3][2] =  dot3(f, eye);
    return m;
}

/// Build an orthographic projection matrix (Vulkan clip space: z in [0,1])
float4x4 ortho(float left, float right, float bottom, float top,
               float near_val, float far_val) {
    float4x4 m{};
    m.m[0][0] = 2.0f / (right - left);
    m.m[1][1] = 2.0f / (top - bottom);
    m.m[2][2] = -1.0f / (far_val - near_val);  // Vulkan: z in [0,1]
    m.m[3][0] = -(right + left) / (right - left);
    m.m[3][1] = -(top + bottom) / (top - bottom);
    m.m[3][2] = -near_val / (far_val - near_val);
    m.m[3][3] = 1.0f;
    return m;
}

/// Transform a point by a 4x4 matrix (w=1)
float3 transform_point(const float4x4& m, const float3& p) {
    float x = m.m[0][0]*p.x + m.m[1][0]*p.y + m.m[2][0]*p.z + m.m[3][0];
    float y = m.m[0][1]*p.x + m.m[1][1]*p.y + m.m[2][1]*p.z + m.m[3][1];
    float z = m.m[0][2]*p.x + m.m[1][2]*p.y + m.m[2][2]*p.z + m.m[3][2];
    float w = m.m[0][3]*p.x + m.m[1][3]*p.y + m.m[2][3]*p.z + m.m[3][3];
    if (std::abs(w) > 1e-8f) {
        x /= w; y /= w; z /= w;
    }
    return {x, y, z};
}

} // anonymous namespace

// ============================================================
// Construction / Destruction
// ============================================================

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

// ============================================================
// Initialization
// ============================================================

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

        // Per-object x 4 cascade depth values
        resources_.shadow_depths =
            buffer_manager_.allocate_instance_data(max_objects * 4 * sizeof(float));

        // Shadow atlas: depth texture array with one layer per cascade
        // Format: DEPTH_32F, resolution x resolution x cascade_count
        uint32_t res = config_.shadow.resolution;
        uint32_t cascades = std::min(config_.shadow.cascade_count, 4u);
        resources_.shadow_atlas =
            buffer_manager_.allocate_instance_data(
                res * res * cascades * sizeof(float));

        // Shadow uniform buffer (ShadowUniformData)
        resources_.shadow_uniforms =
            buffer_manager_.allocate_instance_data(sizeof(ShadowUniformData));
    }

    // Allocate SSAO resources
    if (config_.ssao_enabled) {
        resources_.ssao_kernel =
            buffer_manager_.allocate_instance_data(
                config_.ssao.sample_count * 4 * sizeof(float));
        resources_.ao_output =
            buffer_manager_.allocate_instance_data(screen_width * screen_height);
    }

    // Allocate GI probe resources
    if (config_.gi_probes_enabled) {
        uint32_t total_probes = config_.probes.grid_x
                              * config_.probes.grid_y
                              * config_.probes.grid_z;
        resources_.probe_irradiance =
            buffer_manager_.allocate_instance_data(total_probes * 9 * 4 * sizeof(float));
        resources_.object_irradiance =
            buffer_manager_.allocate_instance_data(max_objects * 9 * 4 * sizeof(float));
    }

    initialized_ = true;
}

// ============================================================
// Frame Execution
// ============================================================

void GILightingSystem::execute(const float4x4& camera_view,
                               const float4x4& camera_projection) {
    if (!initialized_) return;

    uint32_t object_count = registry_.total_object_count();
    if (object_count == 0) return;

    stats_ = {};

    // Pass 1: Shadow map generation
    if (config_.shadow_enabled) {
        execute_shadow_pass(camera_view, camera_projection);
    }

    // Pass 2: Screen-space ambient occlusion
    if (config_.ssao_enabled) {
        execute_ssao_pass(camera_projection);
    }

    // Pass 3: GI probe sampling
    if (config_.gi_probes_enabled) {
        execute_gi_probe_pass();
    }
}

void GILightingSystem::set_directional_light(const DirectionalLight& light) {
    directional_light_ = light;
}

void GILightingSystem::upload_probe_data(const float* sh_data, uint32_t probe_count) {
    if (!initialized_ || !config_.gi_probes_enabled) return;
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
    uint32_t cascade_count = std::min(config_.shadow.cascade_count, 4u);

    // Compute cascade split distances using practical-logarithmic scheme
    compute_cascade_splits(0.1f, config_.shadow.max_shadow_dist,
                           cached_cascade_splits_, cascade_count);

    // Compute per-cascade light-space view-projection matrices
    for (uint32_t c = 0; c < cascade_count; c++) {
        shadow_uniform_data_.shadow_view_proj[c] =
            compute_light_view_proj(camera_view, camera_projection,
                                    cached_cascade_splits_[c],
                                    cached_cascade_splits_[c + 1]);
    }

    // Update shadow uniform data from config
    update_shadow_uniforms(camera_view, camera_projection);

    // Vulkan command recording:
    //   For each cascade:
    //     1. Begin render pass with shadow atlas layer as depth attachment
    //     2. Set viewport/scissor to cascade's atlas region
    //     3. Bind shadow_depth.vert/frag pipeline
    //     4. For each shadow-casting object visible in this cascade:
    //        a. Push ShadowPushConstants (model, cascadeIndex, materialFlags)
    //        b. Bind vertex/index buffers
    //        c. vkCmdDrawIndexed
    //     5. End render pass
    //   Then dispatch shadow_map_gen.comp for per-object cascade assignment

    for (uint32_t c = 0; c < cascade_count; c++) {
        render_shadow_cascade(c, shadow_uniform_data_.shadow_view_proj[c]);
    }

    // Dispatch compute shader for per-object cascade flag assignment
    uint32_t workgroups = calculate_workgroups(object_count);

    stats_.shadow_casters = object_count;
    stats_.active_cascades = cascade_count;
    stats_.gi_workgroups += workgroups;
}

void GILightingSystem::render_shadow_cascade(uint32_t cascade_index,
                                              const float4x4& light_view_proj) {
    // In a real Vulkan implementation:
    //   1. Transition shadow atlas layer to DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    //   2. Begin render pass (depth-only, no color attachment)
    //   3. Bind shadow depth pipeline (shadow_depth.vert + shadow_depth.frag)
    //   4. Set viewport: (0, 0, resolution, resolution)
    //   5. For each object with CAST_SHADOW flag:
    //      - Skip if not visible in this cascade (frustum test vs light frustum)
    //      - Push model matrix, cascade index, alpha test params
    //      - Draw indexed
    //   6. End render pass
    //   7. Transition shadow atlas layer to SHADER_READ_ONLY_OPTIMAL
    (void)cascade_index;
    (void)light_view_proj;
}

// ============================================================
// Shadow Uniform Update
// ============================================================

void GILightingSystem::update_shadow_uniforms(const float4x4& camera_view,
                                               const float4x4& camera_projection) {
    (void)camera_view;
    (void)camera_projection;

    uint32_t cascade_count = std::min(config_.shadow.cascade_count, 4u);
    const auto& cfg = config_.shadow;

    // Copy cascade split depths (view-space far distances)
    for (uint32_t i = 0; i < cascade_count; i++) {
        shadow_uniform_data_.cascade_split_depths[i] = cached_cascade_splits_[i + 1];
    }
    for (uint32_t i = cascade_count; i < 4; i++) {
        shadow_uniform_data_.cascade_split_depths[i] = cfg.max_shadow_dist;
    }

    // Shadow parameters
    shadow_uniform_data_.texel_size = 1.0f / static_cast<float>(cfg.resolution);
    shadow_uniform_data_.cascade_count_f = static_cast<float>(cascade_count);
    shadow_uniform_data_.filter_mode_f = static_cast<float>(cfg.filter_mode);
    shadow_uniform_data_.shadow_strength = cfg.shadow_strength;

    // Bias parameters
    shadow_uniform_data_.depth_bias = cfg.depth_bias;
    shadow_uniform_data_.normal_bias = cfg.normal_bias;
    shadow_uniform_data_.slope_scale_bias = cfg.slope_scale_bias;
    shadow_uniform_data_.cascade_blend_width = cfg.cascade_blend_width;

    // PCSS parameters
    shadow_uniform_data_.pcss_light_size = cfg.pcss_light_size;
    shadow_uniform_data_.pcss_min_penumbra = cfg.pcss_min_penumbra;
    shadow_uniform_data_.pcss_max_penumbra = cfg.pcss_max_penumbra;
    shadow_uniform_data_.pcss_blocker_search_radius = cfg.pcss_blocker_search_radius;

    // In a real Vulkan implementation:
    // Upload shadow_uniform_data_ to the GPU uniform buffer
    // vkCmdUpdateBuffer or staging buffer → shadow_uniforms allocation
}

// ============================================================
// SSAO Pass
// ============================================================

void GILightingSystem::execute_ssao_pass(const float4x4& camera_projection) {
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
// Light-Space View-Projection (Tight-Fit CSM)
// ============================================================

float4x4 GILightingSystem::compute_light_view_proj(
    const float4x4& camera_view,
    const float4x4& camera_projection,
    float near_split, float far_split) const
{
    // Step 1: Compute the 8 corners of the camera sub-frustum
    //         defined by [near_split, far_split] in view space
    float4x4 inv_view_proj = inverse4x4(multiply4x4(camera_view, camera_projection));

    // NDC corners: near plane z=0 (Vulkan), far plane z=1
    float near_ndc = near_split;  // Will be recomputed from projection
    float far_ndc  = far_split;

    // For proper NDC mapping, we need the camera's actual near/far from projection.
    // Use the sub-frustum splits to build a sub-projection.
    // Instead, compute frustum corners in world space directly from inverse VP.

    // Compute normalized depths from the projection matrix
    // For Vulkan perspective: z_ndc = (far * near_split) / (far - near_split * (far - near))
    // Simplification: use the sub-frustum approach

    // NDC corners of the full frustum, then scale z to [near_split, far_split]
    // mapped to normalized device coordinates
    float near_z = 0.0f;  // Vulkan near plane in NDC
    float far_z  = 1.0f;  // Vulkan far plane in NDC

    // For cascade sub-frustum: linearly interpolate in NDC z
    float max_dist = config_.shadow.max_shadow_dist;
    float t_near = near_split / max_dist;
    float t_far  = far_split / max_dist;
    near_z = t_near;
    far_z  = t_far;

    float3 frustum_corners[8];
    int idx = 0;
    for (int z = 0; z < 2; z++) {
        float ndc_z = (z == 0) ? near_z : far_z;
        for (int y = 0; y < 2; y++) {
            for (int x = 0; x < 2; x++) {
                float ndc_x = (x == 0) ? -1.0f : 1.0f;
                float ndc_y = (y == 0) ? -1.0f : 1.0f;
                frustum_corners[idx] = transform_point(inv_view_proj,
                    {ndc_x, ndc_y, ndc_z});
                idx++;
            }
        }
    }

    // Step 2: Compute the centroid of the frustum corners
    float3 center = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 8; i++) {
        center = center + frustum_corners[i];
    }
    center = center * (1.0f / 8.0f);

    // Step 3: Build light view matrix looking from the light direction
    float3 light_dir = normalize3(directional_light_.direction);

    // Choose up vector that isn't parallel to light direction
    float3 up = {0.0f, 1.0f, 0.0f};
    if (std::abs(dot3(light_dir, up)) > 0.99f) {
        up = {1.0f, 0.0f, 0.0f};
    }

    // Light position: place it far enough behind the frustum center
    float3 light_pos = center - light_dir * max_dist;
    float4x4 light_view = look_at(light_pos, center, up);

    // Step 4: Transform frustum corners to light view space
    //         and compute tight AABB
    float min_x =  1e30f, max_x = -1e30f;
    float min_y =  1e30f, max_y = -1e30f;
    float min_z =  1e30f, max_z = -1e30f;

    for (int i = 0; i < 8; i++) {
        float3 lv = transform_point(light_view, frustum_corners[i]);
        min_x = std::min(min_x, lv.x);
        max_x = std::max(max_x, lv.x);
        min_y = std::min(min_y, lv.y);
        max_y = std::max(max_y, lv.y);
        min_z = std::min(min_z, lv.z);
        max_z = std::max(max_z, lv.z);
    }

    // Extend near plane to catch shadow casters behind the camera frustum
    float z_range = max_z - min_z;
    min_z -= z_range * 2.0f;

    // Step 5: Apply texel snapping to reduce shadow edge shimmer
    //         when the camera moves
    float resolution = static_cast<float>(config_.shadow.resolution);
    float world_units_per_texel_x = (max_x - min_x) / resolution;
    float world_units_per_texel_y = (max_y - min_y) / resolution;

    if (world_units_per_texel_x > 0.0f) {
        min_x = std::floor(min_x / world_units_per_texel_x) * world_units_per_texel_x;
        max_x = std::floor(max_x / world_units_per_texel_x) * world_units_per_texel_x;
    }
    if (world_units_per_texel_y > 0.0f) {
        min_y = std::floor(min_y / world_units_per_texel_y) * world_units_per_texel_y;
        max_y = std::floor(max_y / world_units_per_texel_y) * world_units_per_texel_y;
    }

    // Step 6: Build orthographic projection
    float4x4 light_proj = ortho(min_x, max_x, min_y, max_y, min_z, max_z);

    // Step 7: Combine view and projection
    return multiply4x4(light_view, light_proj);
}

uint32_t GILightingSystem::calculate_workgroups(uint32_t count, uint32_t size) const {
    return (count + size - 1) / size;
}

} // namespace pictor
