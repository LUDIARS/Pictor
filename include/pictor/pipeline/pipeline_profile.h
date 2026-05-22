#pragma once

#include "pictor/core/types.h"
#include "pictor/memory/memory_subsystem.h"
#include "pictor/update/update_scheduler.h"
#include "pictor/gpu/gpu_driven_pipeline.h"
#include "pictor/gi/gi_lighting_system.h"
#include "pictor/postprocess/postprocess_effect.h"
#include "pictor/pipeline/attachment_def.h"
#include <string>
#include <vector>

namespace pictor {

/// Shadow configuration (§8.2)
struct ShadowConfig {
    uint32_t         cascade_count   = 3;
    uint32_t         resolution      = 2048;
    ShadowFilterMode filter_mode     = ShadowFilterMode::PCF;
};

/// Post-process effect kind.
///
/// Identifies which `PostProcessConfig` slot a `PostProcessDef` drives.
/// `UNKNOWN` covers effect names that have no host-driven implementation in
/// `PostProcessPipeline` yet (SSAO / TAA / FXAA / VolumetricFog …): they
/// still round-trip through JSON / the editor, but the post-process bridge
/// ignores them (系統B phase 2 territory — see
/// `spec/rendering-extensibility-design.md` §3).
enum class PostProcessKind : uint8_t {
    UNKNOWN       = 0,  ///< Name has no PostProcessConfig mapping
    BLOOM         = 1,  ///< -> PostProcessConfig::bloom
    TONE_MAPPING  = 2,  ///< -> PostProcessConfig::tone_mapping
    VIGNETTE      = 3,  ///< -> PostProcessConfig::vignette
    COLOR_GRADING = 4,  ///< -> PostProcessConfig::color_grading (LUT)
    DEPTH_OF_FIELD = 5, ///< -> PostProcessConfig::depth_of_field
};

/// Post-process effect definition (§8.2).
///
/// Phase 1 of 方針2 (`spec/rendering-extensibility-design.md` §3): besides
/// `name` / `enabled`, the def now carries a typed `kind` and one parameter
/// struct per supported effect. Only the struct matching `kind` is
/// meaningful; the others stay at their defaults. The serializer round-trips
/// the active effect's parameters, and `build_post_process_config()`
/// (`postprocess_config_bridge.h`) folds the stack into a `PostProcessConfig`
/// that drives the real `PostProcessPipeline`.
///
/// `kind` is normally derived from `name` (case-insensitive) via
/// `post_process_kind_from_name()`; an explicit `kind` field in the JSON
/// overrides that inference.
struct PostProcessDef {
    std::string     name;
    bool            enabled = true;
    PostProcessKind kind    = PostProcessKind::UNKNOWN;

    // Effect parameters — only the struct matching `kind` is consumed by the
    // bridge. Defaults mirror `PostProcessConfig`'s per-effect defaults.
    BloomConfig          bloom;
    ToneMappingConfig    tone_mapping;
    VignetteConfig       vignette;
    ColorGradingConfig   color_grading;
    DepthOfFieldConfig   depth_of_field;
};

/// Map an effect name (case-insensitive) to a `PostProcessKind`.
/// Accepts the canonical preset spellings plus common aliases:
///   Bloom, Tonemapping/ToneMapping/Tonemap, Vignette,
///   ColorGrading/LUT/Grade, DoF/DepthOfField.
/// Returns `UNKNOWN` for names with no host-driven implementation.
PostProcessKind post_process_kind_from_name(const std::string& name);

/// Render pass definition (§8.3)
struct RenderPassDef {
    std::string             pass_name;
    PassType                pass_type        = PassType::OPAQUE;
    ShaderHandle            shader_override  = INVALID_MESH;
    std::vector<std::string> render_targets;
    std::vector<std::string> input_textures;
    SortMode                sort_mode        = SortMode::FRONT_TO_BACK;
    uint16_t                filter_mask      = 0xFFFF;
    bool                    gpu_driven_pass  = false;
    std::vector<std::string> required_streams; // prefetch hints

    /// Per-attachment load / store ops, in `render_targets` order (Phase 3,
    /// schema v2). If empty the runtime infers defaults via
    /// `default_attachment_ops(render_targets, attachments)` —
    /// CLEAR/STORE for color, CLEAR/DONT_CARE for depth, PRESENT_SRC_KHR
    /// for swapchain. See `spec/pipeline-system-b-config.md` §3.2.
    std::vector<AttachmentOpsDef> attachment_ops;
};

/// Profiler configuration (§8.2)
struct ProfilerConfig {
    bool        enabled       = true;
    OverlayMode overlay_mode  = OverlayMode::STANDARD;
    uint32_t    max_queries   = 64;
};

/// Pipeline profile definition (§8.2).
/// Corresponds to Lite/Standard/Ultra presets or custom profiles.
struct PipelineProfileDef {
    std::string                profile_name;
    RenderingPath              rendering_path       = RenderingPath::FORWARD_PLUS;

    /// Named attachments used by `render_passes[].render_targets` /
    /// `input_textures`. Phase 3 (schema v2). If empty the runtime
    /// substitutes `default_attachments()` (HDR color / depth / swapchain).
    std::vector<AttachmentDef> attachments;

    std::vector<RenderPassDef> render_passes;
    ShadowConfig               shadow_config;
    std::vector<PostProcessDef> post_process_stack;
    bool                       gpu_driven_enabled   = true;
    bool                       compute_update_enabled = true;
    GPUDrivenConfig            gpu_driven_config;
    MemoryConfig               memory_config;
    UpdateConfig               update_config;
    ProfilerConfig             profiler_config;
    GIConfig                   gi_config;
    uint32_t                   max_lights           = 256;
    uint8_t                    msaa_samples         = 0;
};

/// Pipeline profile manager (§8.4, §12).
/// Provides built-in presets and supports custom profile registration.
class PipelineProfileManager {
public:
    PipelineProfileManager();
    ~PipelineProfileManager();

    /// Register built-in profiles (Lite, Standard, Ultra)
    void register_defaults();

    /// Register a custom profile (§12)
    void register_profile(const PipelineProfileDef& def);

    /// Set active profile by name (§8.4)
    bool set_profile(const std::string& name);

    /// Get current profile
    const PipelineProfileDef& current_profile() const { return *current_; }
    const std::string& current_profile_name() const { return current_->profile_name; }

    /// Get profile by name
    const PipelineProfileDef* get_profile(const std::string& name) const;

    /// List all registered profile names
    std::vector<std::string> profile_names() const;

    /// Create built-in profiles (§8.1)
    static PipelineProfileDef create_lite_profile();
    static PipelineProfileDef create_standard_profile();
    static PipelineProfileDef create_ultra_profile();

    /// Mobile presets (§8.1 extension).
    /// MobileLow targets bandwidth-bound tile-based GPUs (no shadows, FORWARD, no MSAA).
    /// MobileHigh targets modern mobile SoCs (FORWARD, 1-cascade PCF shadows, light post-process).
    static PipelineProfileDef create_mobile_low_profile();
    static PipelineProfileDef create_mobile_high_profile();

private:
    std::vector<PipelineProfileDef> profiles_;
    const PipelineProfileDef*       current_ = nullptr;
};

} // namespace pictor
