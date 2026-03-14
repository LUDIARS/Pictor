#include "pictor/pipeline/pipeline_profile.h"
#include <algorithm>

namespace pictor {

PipelineProfileManager::PipelineProfileManager() = default;
PipelineProfileManager::~PipelineProfileManager() = default;

void PipelineProfileManager::register_defaults() {
    register_profile(create_lite_profile());
    register_profile(create_standard_profile());
    register_profile(create_ultra_profile());
    set_profile("Standard");
}

void PipelineProfileManager::register_profile(const PipelineProfileDef& def) {
    // Replace if exists
    for (auto& p : profiles_) {
        if (p.profile_name == def.profile_name) {
            p = def;
            if (current_ && current_->profile_name == def.profile_name) {
                current_ = &p;
            }
            return;
        }
    }
    profiles_.push_back(def);
    if (!current_) {
        current_ = &profiles_.back();
    }
}

bool PipelineProfileManager::set_profile(const std::string& name) {
    for (const auto& p : profiles_) {
        if (p.profile_name == name) {
            current_ = &p;
            return true;
        }
    }
    return false;
}

const PipelineProfileDef* PipelineProfileManager::get_profile(const std::string& name) const {
    for (const auto& p : profiles_) {
        if (p.profile_name == name) return &p;
    }
    return nullptr;
}

std::vector<std::string> PipelineProfileManager::profile_names() const {
    std::vector<std::string> names;
    names.reserve(profiles_.size());
    for (const auto& p : profiles_) {
        names.push_back(p.profile_name);
    }
    return names;
}

// ============================================================
// Built-in Profile Definitions (§8.1)
// ============================================================

PipelineProfileDef PipelineProfileManager::create_lite_profile() {
    PipelineProfileDef def;
    def.profile_name = "Lite";
    def.rendering_path = RenderingPath::FORWARD;
    def.gpu_driven_enabled = false;
    def.compute_update_enabled = false;
    def.max_lights = 16;
    def.msaa_samples = 2;

    // Shadow config (§8.1: 1 cascade, hard shadows for performance)
    def.shadow_config.cascade_count = 1;
    def.shadow_config.resolution = 1024;
    def.shadow_config.filter_mode = ShadowFilterMode::NONE;

    // Memory config (§4.4: Pictor Lite)
    def.memory_config.frame_allocator_size = 4 * 1024 * 1024; // 4MB
    def.memory_config.flight_count = 2;
    def.gpu_driven_config = {};

    // Update config (§5.5: small scale only)
    def.update_config.chunk_size = 16384;
    def.update_config.nt_store_enabled = false;

    // Profiler
    def.profiler_config.enabled = true;
    def.profiler_config.overlay_mode = OverlayMode::MINIMAL;

    // GI config (Lite: shadows only, no SSAO/probes)
    def.gi_config.shadow_enabled = true;
    def.gi_config.ssao_enabled = false;
    def.gi_config.gi_probes_enabled = false;
    def.gi_config.shadow.cascade_count = 1;
    def.gi_config.shadow.resolution = 1024;
    def.gi_config.shadow.depth_bias = 0.005f;
    def.gi_config.shadow.filter_mode = ShadowFilterMode::NONE;

    // Render passes (§9.1 simplified)
    def.render_passes = {
        {"ShadowPass",      PassType::SHADOW,      INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, false, {"bounds", "transforms"}},
        {"DepthPrePass",    PassType::DEPTH_ONLY,   INVALID_MESH, {}, {}, SortMode::FRONT_TO_BACK, 0xFFFF, false, {"bounds", "transforms"}},
        {"OpaquePass",      PassType::OPAQUE,       INVALID_MESH, {}, {}, SortMode::FRONT_TO_BACK, 0xFFFF, false, {"transforms", "shaderKeys"}},
        {"TransparentPass", PassType::TRANSPARENT,  INVALID_MESH, {}, {}, SortMode::BACK_TO_FRONT, 0xFFFF, false, {"transforms", "sortKeys"}},
        {"PostProcess",     PassType::POST_PROCESS, INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, false, {}},
    };

    // Post-process (§8.1: minimal)
    def.post_process_stack = {
        {"Tonemapping", true},
        {"FXAA", true},
    };

    return def;
}

PipelineProfileDef PipelineProfileManager::create_standard_profile() {
    PipelineProfileDef def;
    def.profile_name = "Standard";
    def.rendering_path = RenderingPath::FORWARD_PLUS;
    def.gpu_driven_enabled = true;
    def.compute_update_enabled = false;
    def.max_lights = 256;
    def.msaa_samples = 4;

    // Shadow config (§8.1: 3 cascades, PCF soft shadows)
    def.shadow_config.cascade_count = 3;
    def.shadow_config.resolution = 2048;
    def.shadow_config.filter_mode = ShadowFilterMode::PCF;

    // Memory config (§4.4: Pictor Standard)
    def.memory_config.frame_allocator_size = 16 * 1024 * 1024; // 16MB
    def.memory_config.flight_count = 3;
    def.memory_config.gpu_config.mesh_pool_size = 256 * 1024 * 1024;
    def.memory_config.gpu_config.ssbo_pool_size = 128 * 1024 * 1024;

    // GPU Driven config
    def.gpu_driven_config.max_triangle_count = 50000;
    def.gpu_driven_config.min_instance_count = 32;
    def.gpu_driven_config.compute_update = false;

    // Update config
    def.update_config.chunk_size = 16384;
    def.update_config.nt_store_enabled = true;
    def.update_config.nt_store_threshold = 10000;

    // Profiler
    def.profiler_config.enabled = true;
    def.profiler_config.overlay_mode = OverlayMode::STANDARD;

    // GI config (Standard: shadows + SSAO)
    def.gi_config.shadow_enabled = true;
    def.gi_config.ssao_enabled = true;
    def.gi_config.gi_probes_enabled = false;
    def.gi_config.shadow.cascade_count = 3;
    def.gi_config.shadow.resolution = 2048;
    def.gi_config.shadow.filter_mode = ShadowFilterMode::PCF;
    def.gi_config.ssao.sample_count = 32;
    def.gi_config.ssao.radius = 0.5f;
    def.gi_config.ssao.intensity = 1.0f;

    // Render passes (§9.1: Pictor Standard)
    def.render_passes = {
        {"ShadowPass",      PassType::SHADOW,       INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, false, {"bounds", "transforms"}},
        {"DepthPrePass",    PassType::DEPTH_ONLY,    INVALID_MESH, {}, {}, SortMode::FRONT_TO_BACK, 0xFFFF, false, {"bounds", "transforms"}},
        {"HiZBuild",        PassType::COMPUTE,       INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, false, {}},
        {"GPUCullPass",     PassType::COMPUTE,       INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, true, {"gpu_bounds"}},
        {"ShadowMapGen",    PassType::COMPUTE,       INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, false, {"gpu_bounds", "gpu_transforms"}},
        {"SSAOGen",         PassType::COMPUTE,       INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, false, {}},
        {"OpaquePass",      PassType::OPAQUE,        INVALID_MESH, {}, {}, SortMode::FRONT_TO_BACK, 0xFFFF, false, {"transforms", "shaderKeys"}},
        {"SkyboxPass",      PassType::CUSTOM,        INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, false, {}},
        {"TransparentPass", PassType::TRANSPARENT,   INVALID_MESH, {}, {}, SortMode::BACK_TO_FRONT, 0xFFFF, false, {"transforms", "sortKeys"}},
        {"PostProcess",     PassType::POST_PROCESS,  INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, false, {}},
    };

    // Post-process (§8.1: SSAO + Bloom + Tonemap + TAA)
    def.post_process_stack = {
        {"SSAO", true},
        {"Bloom", true},
        {"Tonemapping", true},
        {"TAA", true},
    };

    return def;
}

PipelineProfileDef PipelineProfileManager::create_ultra_profile() {
    PipelineProfileDef def;
    def.profile_name = "Ultra";
    def.rendering_path = RenderingPath::HYBRID;
    def.gpu_driven_enabled = true;
    def.compute_update_enabled = true;
    def.max_lights = 1024;
    def.msaa_samples = 0; // TAA handles anti-aliasing

    // Shadow config (§8.1: high quality, PCSS contact-hardening shadows)
    def.shadow_config.cascade_count = 4;
    def.shadow_config.resolution = 4096;
    def.shadow_config.filter_mode = ShadowFilterMode::PCSS;

    // Memory config (§4.4: Pictor Ultra)
    def.memory_config.frame_allocator_size = 64 * 1024 * 1024; // 64MB
    def.memory_config.flight_count = 3;
    def.memory_config.gpu_config.mesh_pool_size = 1024 * 1024 * 1024;   // 1GB
    def.memory_config.gpu_config.ssbo_pool_size = 512 * 1024 * 1024;    // 512MB
    def.memory_config.gpu_config.instance_buffer_size = 256 * 1024 * 1024;
    def.memory_config.gpu_config.staging_buffer_size = 192 * 1024 * 1024; // Triple buffer

    // GPU Driven config (§7)
    def.gpu_driven_config.max_triangle_count = 50000;
    def.gpu_driven_config.min_instance_count = 32;
    def.gpu_driven_config.workgroup_size = 256;
    def.gpu_driven_config.two_phase_culling = true;
    def.gpu_driven_config.compute_update = true;

    // Update config (§5)
    def.update_config.chunk_size = 16384;
    def.update_config.nt_store_enabled = true;
    def.update_config.nt_store_threshold = 10000;

    // Profiler
    def.profiler_config.enabled = true;
    def.profiler_config.overlay_mode = OverlayMode::DETAILED;
    def.profiler_config.max_queries = 64;

    // GI config (Ultra: shadows + SSAO + GI probes)
    def.gi_config.shadow_enabled = true;
    def.gi_config.ssao_enabled = true;
    def.gi_config.gi_probes_enabled = true;
    def.gi_config.shadow.cascade_count = 4;
    def.gi_config.shadow.resolution = 4096;
    def.gi_config.shadow.cascade_lambda = 0.8f;
    def.gi_config.shadow.max_shadow_dist = 300.0f;
    def.gi_config.shadow.filter_mode = ShadowFilterMode::PCSS;
    def.gi_config.shadow.pcss_light_size = 0.06f;
    def.gi_config.shadow.pcss_max_penumbra = 24.0f;
    def.gi_config.ssao.sample_count = 64;
    def.gi_config.ssao.radius = 0.5f;
    def.gi_config.ssao.intensity = 1.2f;
    def.gi_config.probes.grid_x = 32;
    def.gi_config.probes.grid_y = 16;
    def.gi_config.probes.grid_z = 32;
    def.gi_config.probes.gi_intensity = 1.0f;

    // Render passes (§9.2: Pictor Ultra with Compute Update)
    def.render_passes = {
        {"ComputeUpdate",   PassType::COMPUTE,       INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, true, {"gpu_velocities", "gpu_transforms", "gpu_bounds"}},
        {"ShadowPass",      PassType::SHADOW,        INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, false, {"gpu_transforms"}},
        {"DepthPrePass",    PassType::DEPTH_ONLY,    INVALID_MESH, {}, {}, SortMode::FRONT_TO_BACK, 0xFFFF, false, {"gpu_transforms"}},
        {"HiZBuild",        PassType::COMPUTE,       INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, false, {}},
        {"GPUCullPass",     PassType::COMPUTE,       INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, true, {"gpu_bounds"}},
        {"GPULODCompact",   PassType::COMPUTE,       INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, true, {"gpu_transforms", "gpu_mesh_info"}},
        {"ShadowMapGen",    PassType::COMPUTE,       INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, false, {"gpu_bounds", "gpu_transforms"}},
        {"SSAOGen",         PassType::COMPUTE,       INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, false, {}},
        {"GIProbePass",     PassType::COMPUTE,       INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, false, {"gpu_transforms"}},
        {"GBufferPass",     PassType::OPAQUE,        INVALID_MESH, {}, {}, SortMode::FRONT_TO_BACK, 0xFFFF, true, {"gpu_transforms", "gpu_material_ids"}},
        {"LightingPass",    PassType::COMPUTE,       INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, false, {}},
        {"TransparentPass", PassType::TRANSPARENT,   INVALID_MESH, {}, {}, SortMode::BACK_TO_FRONT, 0xFFFF, false, {"gpu_transforms", "sortKeys"}},
        {"PostProcess",     PassType::POST_PROCESS,  INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, false, {}},
    };

    // Post-process (§8.1: Full Ultra)
    def.post_process_stack = {
        {"SSAO", true},
        {"Bloom", true},
        {"Tonemapping", true},
        {"TAA", true},
        {"VolumetricFog", true},
    };

    return def;
}

} // namespace pictor
