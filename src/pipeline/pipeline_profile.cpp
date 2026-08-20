#include "pictor/pipeline/pipeline_profile.h"
#include <algorithm>
#include <cctype>
#include <string>

namespace pictor {

namespace {

/// ASCII-lowercase a copy of `s` for case-insensitive name matching.
std::string ascii_lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

/// Resolve `kind` from `name` for every effect whose kind is still UNKNOWN.
/// Factory presets use positional aggregate init (`{"Bloom", true}`), which
/// leaves `kind` defaulted; this lets the serializer and the bridge see the
/// proper typed kind without each preset having to spell it out.
void normalize_post_process_kinds(PipelineProfileDef& def) {
    for (auto& pp : def.post_process_stack) {
        if (pp.kind == PostProcessKind::UNKNOWN) {
            pp.kind = post_process_kind_from_name(pp.name);
        }
    }
}

} // namespace

PostProcessKind post_process_kind_from_name(const std::string& name) {
    const std::string n = ascii_lower(name);
    if (n == "bloom")                                  return PostProcessKind::BLOOM;
    if (n == "tonemapping" || n == "tonemap" ||
        n == "tone_mapping")                           return PostProcessKind::TONE_MAPPING;
    if (n == "vignette")                               return PostProcessKind::VIGNETTE;
    if (n == "colorgrading" || n == "color_grading" ||
        n == "lut" || n == "grade")                    return PostProcessKind::COLOR_GRADING;
    if (n == "dof" || n == "depthoffield" ||
        n == "depth_of_field")                         return PostProcessKind::DEPTH_OF_FIELD;
    if (n == "ssao" || n == "ambientocclusion" ||
        n == "ambient_occlusion")                      return PostProcessKind::SSAO;
    if (n == "motionblur" || n == "motion_blur")       return PostProcessKind::MOTION_BLUR;
    if (n == "fxaa" || n == "antialiasing" ||
        n == "anti_aliasing")                          return PostProcessKind::FXAA;
    if (n == "chromaticaberration" ||
        n == "chromatic_aberration" || n == "ca")      return PostProcessKind::CHROMATIC_ABERRATION;
    if (n == "filmgrain" || n == "film_grain" ||
        n == "grain")                                  return PostProcessKind::FILM_GRAIN;
    if (n == "taa" || n == "temporalaa" ||
        n == "temporal_aa")                            return PostProcessKind::TAA;
    if (n == "ssr" || n == "screenspacereflections" ||
        n == "screen_space_reflections")               return PostProcessKind::SSR;
    return PostProcessKind::UNKNOWN;
}

// ============================================================
// パイプライン途中編集 API (§12 拡張)
// ============================================================

namespace {

/// 名前付き要素列から `name` の index を返す共通実装。 見つからなければ -1。
template <typename Vec, typename NameOf>
int find_by_name(const Vec& v, std::string_view name, NameOf name_of) {
    for (size_t i = 0; i < v.size(); ++i) {
        if (name_of(v[i]) == name) return static_cast<int>(i);
    }
    return -1;
}

/// anchor の直前 / 直後へ挿入する共通実装。 anchor 不在 / 同名重複は false。
template <typename Vec, typename Elem, typename NameOf>
bool insert_by_anchor(Vec& v, std::string_view anchor, Elem elem, bool after,
                      NameOf name_of) {
    const int at = find_by_name(v, anchor, name_of);
    if (at < 0) return false;
    if (find_by_name(v, name_of(elem), name_of) >= 0) return false;
    v.insert(v.begin() + at + (after ? 1 : 0), std::move(elem));
    return true;
}

} // namespace

int find_render_pass(const PipelineProfileDef& def, std::string_view pass_name) {
    return find_by_name(def.render_passes, pass_name,
                        [](const RenderPassDef& p) -> const std::string& {
                            return p.pass_name;
                        });
}

bool insert_render_pass_before(PipelineProfileDef& def, std::string_view anchor,
                               RenderPassDef pass) {
    return insert_by_anchor(def.render_passes, anchor, std::move(pass), false,
                            [](const RenderPassDef& p) -> const std::string& {
                                return p.pass_name;
                            });
}

bool insert_render_pass_after(PipelineProfileDef& def, std::string_view anchor,
                              RenderPassDef pass) {
    return insert_by_anchor(def.render_passes, anchor, std::move(pass), true,
                            [](const RenderPassDef& p) -> const std::string& {
                                return p.pass_name;
                            });
}

bool remove_render_pass(PipelineProfileDef& def, std::string_view pass_name) {
    const int at = find_render_pass(def, pass_name);
    if (at < 0) return false;
    def.render_passes.erase(def.render_passes.begin() + at);
    return true;
}

int find_post_process(const PipelineProfileDef& def, std::string_view name) {
    return find_by_name(def.post_process_stack, name,
                        [](const PostProcessDef& p) -> const std::string& {
                            return p.name;
                        });
}

bool insert_post_process_before(PipelineProfileDef& def, std::string_view anchor,
                                PostProcessDef effect) {
    if (effect.kind == PostProcessKind::UNKNOWN) {
        effect.kind = post_process_kind_from_name(effect.name);
    }
    return insert_by_anchor(def.post_process_stack, anchor, std::move(effect),
                            false,
                            [](const PostProcessDef& p) -> const std::string& {
                                return p.name;
                            });
}

bool insert_post_process_after(PipelineProfileDef& def, std::string_view anchor,
                               PostProcessDef effect) {
    if (effect.kind == PostProcessKind::UNKNOWN) {
        effect.kind = post_process_kind_from_name(effect.name);
    }
    return insert_by_anchor(def.post_process_stack, anchor, std::move(effect),
                            true,
                            [](const PostProcessDef& p) -> const std::string& {
                                return p.name;
                            });
}

bool remove_post_process(PipelineProfileDef& def, std::string_view name) {
    const int at = find_post_process(def, name);
    if (at < 0) return false;
    def.post_process_stack.erase(def.post_process_stack.begin() + at);
    return true;
}

PipelineProfileManager::PipelineProfileManager() = default;
PipelineProfileManager::~PipelineProfileManager() = default;

void PipelineProfileManager::register_defaults() {
    // 品質 3 段階の既定パイプライン (§8.1 拡張)。
    register_profile(create_low_profile());
    register_profile(create_mid_profile());
    register_profile(create_high_profile());
    // 従来プリセット (§8.1)。
    register_profile(create_lite_profile());
    register_profile(create_standard_profile());
    register_profile(create_ultra_profile());
    set_profile("Standard");
}

void PipelineProfileManager::register_profile(const PipelineProfileDef& def) {
    // Replace if exists (index 保持なので current_ はそのまま有効)
    for (auto& p : profiles_) {
        if (p.profile_name == def.profile_name) {
            p = def;
            return;
        }
    }
    profiles_.push_back(def);
    if (current_ == kNoProfile) {
        current_ = profiles_.size() - 1;
    }
}

bool PipelineProfileManager::set_profile(const std::string& name) {
    for (size_t i = 0; i < profiles_.size(); ++i) {
        if (profiles_[i].profile_name == name) {
            current_ = i;
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
        {"OpaquePass",      PassType::OPAQUE,       INVALID_MESH, {}, {}, SortMode::FRONT_TO_BACK, RenderBatchFilter::OPAQUE, false, {"transforms", "shaderKeys"}},
        {"TransparentPass", PassType::TRANSPARENT,  INVALID_MESH, {}, {}, SortMode::BACK_TO_FRONT, RenderBatchFilter::TRANSPARENT, false, {"transforms", "sortKeys"}},
        {"PostProcess",     PassType::POST_PROCESS, INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, false, {}},
    };

    // Post-process (§8.1: minimal)
    def.post_process_stack = {
        {"Tonemapping", true},
        {"FXAA", true},
    };

    normalize_post_process_kinds(def);
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
        {"OpaquePass",      PassType::OPAQUE,        INVALID_MESH, {}, {}, SortMode::FRONT_TO_BACK, RenderBatchFilter::OPAQUE, false, {"transforms", "shaderKeys"}},
        {"SkyboxPass",      PassType::CUSTOM,        INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, false, {}},
        {"TransparentPass", PassType::TRANSPARENT,   INVALID_MESH, {}, {}, SortMode::BACK_TO_FRONT, RenderBatchFilter::TRANSPARENT, false, {"transforms", "sortKeys"}},
        {"PostProcess",     PassType::POST_PROCESS,  INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, false, {}},
    };

    // Post-process (§8.1: SSAO + Bloom + Tonemap + TAA)
    def.post_process_stack = {
        {"SSAO", true},
        {"Bloom", true},
        {"Tonemapping", true},
        {"TAA", true},
    };

    normalize_post_process_kinds(def);
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
        {"GBufferPass",     PassType::OPAQUE,        INVALID_MESH, {}, {}, SortMode::FRONT_TO_BACK, RenderBatchFilter::OPAQUE, true, {"gpu_transforms", "gpu_material_ids"}},
        {"LightingPass",    PassType::COMPUTE,       INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, false, {}},
        {"TransparentPass", PassType::TRANSPARENT,   INVALID_MESH, {}, {}, SortMode::BACK_TO_FRONT, RenderBatchFilter::TRANSPARENT, false, {"gpu_transforms", "sortKeys"}},
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

    normalize_post_process_kinds(def);
    return def;
}

// ============================================================
// Quality Tier Definitions — Low / Mid / High (§8.1 拡張)
//
// 既定パイプラインの品質 3 段階。 Mid は Low から、 High は Mid から
// 途中編集 API (insert_render_pass_* / insert_post_process_*) で段階的に
// 組み上げる — 「パイプラインを途中で変える」 経路そのものを既定
// プリセットの構築に使うことで、 API とプリセットの整合を保証する。
// ============================================================

PipelineProfileDef PipelineProfileManager::create_low_profile() {
    PipelineProfileDef def;
    def.profile_name = "Low";
    def.rendering_path = RenderingPath::FORWARD;
    def.gpu_driven_enabled = false;
    def.compute_update_enabled = false;
    def.max_lights = 16;
    def.msaa_samples = 0;

    // 影なし — Low は最小構成 (forward 1 経路 + tonemap のみ)。
    def.shadow_config.cascade_count = 0;
    def.shadow_config.resolution    = 0;
    def.shadow_config.filter_mode   = ShadowFilterMode::NONE;

    def.memory_config.frame_allocator_size = 4 * 1024 * 1024; // 4MB
    def.memory_config.flight_count = 2;
    def.gpu_driven_config = {};
    def.gpu_driven_config.compute_update = false;

    def.update_config.chunk_size = 16384;
    def.update_config.nt_store_enabled = false;

    def.profiler_config.enabled      = true;
    def.profiler_config.overlay_mode = OverlayMode::MINIMAL;

    // GI: 全て off。
    def.gi_config.shadow_enabled    = false;
    def.gi_config.ssao_enabled      = false;
    def.gi_config.gi_probes_enabled = false;

    def.render_passes = {
        {"OpaquePass",      PassType::OPAQUE,       INVALID_MESH, {}, {}, SortMode::FRONT_TO_BACK, RenderBatchFilter::OPAQUE, false, {"transforms", "shaderKeys"}},
        {"TransparentPass", PassType::TRANSPARENT,  INVALID_MESH, {}, {}, SortMode::BACK_TO_FRONT, RenderBatchFilter::TRANSPARENT, false, {"transforms", "sortKeys"}},
        {"PostProcess",     PassType::POST_PROCESS, INVALID_MESH, {}, {}, SortMode::NONE,          0xFFFF, false, {}},
    };

    def.post_process_stack = {
        {"Tonemapping", true},
    };

    normalize_post_process_kinds(def);
    return def;
}

PipelineProfileDef PipelineProfileManager::create_mid_profile() {
    // Low を種に、 影 2 pass とポスト (Bloom / Vignette) を途中挿入する。
    PipelineProfileDef def = create_low_profile();
    def.profile_name = "Mid";
    def.max_lights   = 64;
    def.msaa_samples = 2;

    // 影: 2 cascade / PCF。
    def.shadow_config.cascade_count = 2;
    def.shadow_config.resolution    = 2048;
    def.shadow_config.filter_mode   = ShadowFilterMode::PCF;
    def.gi_config.shadow_enabled          = true;
    def.gi_config.shadow.cascade_count    = 2;
    def.gi_config.shadow.resolution       = 2048;
    def.gi_config.shadow.filter_mode      = ShadowFilterMode::PCF;
    def.gi_config.shadow.depth_bias       = 0.005f;

    def.memory_config.frame_allocator_size = 8 * 1024 * 1024; // 8MB
    def.memory_config.flight_count = 3;
    def.memory_config.gpu_config.mesh_pool_size = 128 * 1024 * 1024;
    def.memory_config.gpu_config.ssbo_pool_size = 64 * 1024 * 1024;

    def.profiler_config.overlay_mode = OverlayMode::STANDARD;

    // Opaque の前段へ Shadow → DepthPre を途中挿入。
    insert_render_pass_before(def, "OpaquePass",
        {"ShadowPass",   PassType::SHADOW,     INVALID_MESH, {}, {}, SortMode::NONE,          0xFFFF, false, {"bounds", "transforms"}});
    insert_render_pass_before(def, "OpaquePass",
        {"DepthPrePass", PassType::DEPTH_ONLY, INVALID_MESH, {}, {}, SortMode::FRONT_TO_BACK, 0xFFFF, false, {"bounds", "transforms"}});

    // ポスト: Bloom を Tonemapping の前、 Vignette を後へ (Mid = Bloom 等が
    // 載るパイプライン)。
    insert_post_process_before(def, "Tonemapping", {"Bloom", true});
    insert_post_process_after(def, "Tonemapping", {"Vignette", true});

    normalize_post_process_kinds(def);
    return def;
}

PipelineProfileDef PipelineProfileManager::create_high_profile() {
    // Mid を種に、 Forward+ 相当の pass 列 (DepthPre → LightCull → shading)
    // と DoF を途中挿入する。
    PipelineProfileDef def = create_mid_profile();
    def.profile_name = "High";
    def.rendering_path = RenderingPath::FORWARD_PLUS;
    def.gpu_driven_enabled = true;
    def.max_lights   = 256;
    def.msaa_samples = 0; // フル解像度ポスト前提 (AA はポスト側の領分)

    // 影: 3 cascade へ増強。
    def.shadow_config.cascade_count    = 3;
    def.gi_config.shadow.cascade_count = 3;

    // SSAO on (深度 prepass があるので流用できる)。
    def.gi_config.ssao_enabled      = true;
    def.gi_config.ssao.sample_count = 32;
    def.gi_config.ssao.radius       = 0.5f;
    def.gi_config.ssao.intensity    = 1.0f;

    def.memory_config.frame_allocator_size = 16 * 1024 * 1024; // 16MB
    def.memory_config.gpu_config.mesh_pool_size = 256 * 1024 * 1024;
    def.memory_config.gpu_config.ssbo_pool_size = 128 * 1024 * 1024;

    def.gpu_driven_config.max_triangle_count = 50000;
    def.gpu_driven_config.min_instance_count = 32;

    // Forward+ 相当: DepthPrePass の直後へタイル光源カリングの compute を
    // 途中挿入する (depth prepass → light cull → forward shading)。
    insert_render_pass_after(def, "DepthPrePass",
        {"LightCullPass", PassType::COMPUTE, INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, false, {}});
    insert_render_pass_after(def, "LightCullPass",
        {"SSAOGen",       PassType::COMPUTE, INVALID_MESH, {}, {}, SortMode::NONE, 0xFFFF, false, {}});

    // ポスト: DoF を Bloom の前へ (チェーン上も dof → bloom → grade の順で
    // 深度をパイプラインへ捩じ込む — `build_post_process_chain()` 参照)。
    PostProcessDef dof;
    dof.name    = "DepthOfField";
    dof.enabled = true;
    dof.depth_of_field.enabled = true;
    insert_post_process_before(def, "Bloom", dof);

    normalize_post_process_kinds(def);
    return def;
}

// ============================================================
// Mobile Profile Definitions (§8.1 extension)
//
// These presets are tuned for mobile tile-based GPUs (Adreno, Mali, Apple
// A-series). Compared to Lite, they disable bandwidth-heavy features
// (MSAA resolve, multi-cascade shadows) that hurt tile-based deferred
// hardware, and shrink memory footprints to fit mobile DRAM budgets.
// ============================================================

PipelineProfileDef PipelineProfileManager::create_mobile_low_profile() {
    PipelineProfileDef def;
    def.profile_name = "MobileLow";
    def.rendering_path = RenderingPath::FORWARD;
    def.gpu_driven_enabled = false;
    def.compute_update_enabled = false;
    def.max_lights = 8;
    def.msaa_samples = 0; // MSAA resolve is expensive on tile-based GPUs

    // Shadows disabled entirely for low-end mobile
    def.shadow_config.cascade_count = 0;
    def.shadow_config.resolution    = 0;
    def.shadow_config.filter_mode   = ShadowFilterMode::NONE;

    // Memory config: sized for ~1-2GB mobile RAM
    def.memory_config.frame_allocator_size = 1 * 1024 * 1024; // 1MB
    def.memory_config.flight_count = 2;
    def.memory_config.gpu_config.mesh_pool_size     = 32 * 1024 * 1024;  // 32MB
    def.memory_config.gpu_config.ssbo_pool_size     = 16 * 1024 * 1024;  // 16MB
    def.memory_config.gpu_config.instance_buffer_size = 4 * 1024 * 1024; // 4MB
    def.memory_config.gpu_config.staging_buffer_size  = 4 * 1024 * 1024; // 4MB

    def.gpu_driven_config = {};
    def.gpu_driven_config.compute_update = false;

    def.update_config.chunk_size = 4096;
    def.update_config.nt_store_enabled = false;

    def.profiler_config.enabled      = true;
    def.profiler_config.overlay_mode = OverlayMode::MINIMAL;
    def.profiler_config.max_queries  = 16;

    // GI: everything off — no shadows, no SSAO, no probes
    def.gi_config.shadow_enabled    = false;
    def.gi_config.ssao_enabled      = false;
    def.gi_config.gi_probes_enabled = false;

    // Minimal render passes — single forward pass + tonemap
    def.render_passes = {
        {"OpaquePass",      PassType::OPAQUE,       INVALID_MESH, {}, {}, SortMode::FRONT_TO_BACK, RenderBatchFilter::OPAQUE, false, {"transforms", "shaderKeys"}},
        {"TransparentPass", PassType::TRANSPARENT,  INVALID_MESH, {}, {}, SortMode::BACK_TO_FRONT, RenderBatchFilter::TRANSPARENT, false, {"transforms", "sortKeys"}},
        {"PostProcess",     PassType::POST_PROCESS, INVALID_MESH, {}, {}, SortMode::NONE,          0xFFFF, false, {}},
    };

    def.post_process_stack = {
        {"Tonemapping", true},
    };

    normalize_post_process_kinds(def);
    return def;
}

PipelineProfileDef PipelineProfileManager::create_mobile_high_profile() {
    PipelineProfileDef def;
    def.profile_name = "MobileHigh";
    def.rendering_path = RenderingPath::FORWARD; // stay on Forward — Forward+ light lists cost bandwidth
    def.gpu_driven_enabled = false;
    def.compute_update_enabled = false;
    def.max_lights = 32;
    def.msaa_samples = 2; // 2x MSAA is the mobile sweet spot

    // 1 shadow cascade, modest resolution, PCF soft shadows
    def.shadow_config.cascade_count = 1;
    def.shadow_config.resolution    = 1024;
    def.shadow_config.filter_mode   = ShadowFilterMode::PCF;

    // Memory config: sized for mid/high-tier mobile (~4GB RAM devices)
    def.memory_config.frame_allocator_size = 4 * 1024 * 1024;  // 4MB
    def.memory_config.flight_count = 2;
    def.memory_config.gpu_config.mesh_pool_size     = 128 * 1024 * 1024; // 128MB
    def.memory_config.gpu_config.ssbo_pool_size     = 32 * 1024 * 1024;  // 32MB
    def.memory_config.gpu_config.instance_buffer_size = 16 * 1024 * 1024;
    def.memory_config.gpu_config.staging_buffer_size  = 16 * 1024 * 1024;

    def.gpu_driven_config = {};
    def.gpu_driven_config.compute_update = false;

    def.update_config.chunk_size = 8192;
    def.update_config.nt_store_enabled = false;

    def.profiler_config.enabled      = true;
    def.profiler_config.overlay_mode = OverlayMode::STANDARD;
    def.profiler_config.max_queries  = 32;

    def.gi_config.shadow_enabled    = true;
    def.gi_config.ssao_enabled      = false; // SSAO is heavy on tile-based GPUs
    def.gi_config.gi_probes_enabled = false;
    def.gi_config.shadow.cascade_count = 1;
    def.gi_config.shadow.resolution    = 1024;
    def.gi_config.shadow.filter_mode   = ShadowFilterMode::PCF;
    def.gi_config.shadow.depth_bias    = 0.005f;

    def.render_passes = {
        {"ShadowPass",      PassType::SHADOW,       INVALID_MESH, {}, {}, SortMode::NONE,          0xFFFF, false, {"bounds", "transforms"}},
        {"DepthPrePass",    PassType::DEPTH_ONLY,   INVALID_MESH, {}, {}, SortMode::FRONT_TO_BACK, 0xFFFF, false, {"bounds", "transforms"}},
        {"OpaquePass",      PassType::OPAQUE,       INVALID_MESH, {}, {}, SortMode::FRONT_TO_BACK, RenderBatchFilter::OPAQUE, false, {"transforms", "shaderKeys"}},
        {"TransparentPass", PassType::TRANSPARENT,  INVALID_MESH, {}, {}, SortMode::BACK_TO_FRONT, RenderBatchFilter::TRANSPARENT, false, {"transforms", "sortKeys"}},
        {"PostProcess",     PassType::POST_PROCESS, INVALID_MESH, {}, {}, SortMode::NONE,          0xFFFF, false, {}},
    };

    def.post_process_stack = {
        {"Bloom",       true},
        {"Tonemapping", true},
        {"FXAA",        true},
    };
    normalize_post_process_kinds(def);

    return def;
}

} // namespace pictor
