#pragma once

#include "pictor/core/types.h"
#include "pictor/memory/memory_subsystem.h"
#include "pictor/update/update_scheduler.h"
#include "pictor/gpu/gpu_driven_pipeline.h"
#include "pictor/gi/gi_lighting_system.h"
#include "pictor/postprocess/postprocess_effect.h"
#include "pictor/pipeline/attachment_def.h"
#include <string>
#include <string_view>
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
    SSAO          = 6,  ///< -> PostProcessConfig::ssao (PP 近似)
    MOTION_BLUR   = 7,  ///< -> PostProcessConfig::motion_blur (カメラ再投影)
    FXAA          = 8,  ///< -> PostProcessConfig::fxaa
    CHROMATIC_ABERRATION = 9,  ///< -> PostProcessConfig::chromatic_aberration
    FILM_GRAIN    = 10, ///< -> PostProcessConfig::film_grain
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
    BloomConfig               bloom;
    ToneMappingConfig         tone_mapping;
    VignetteConfig            vignette;
    ColorGradingConfig        color_grading;
    DepthOfFieldConfig        depth_of_field;
    SSAOPostConfig            ssao;
    MotionBlurConfig          motion_blur;
    FXAAConfig                fxaa;
    ChromaticAberrationConfig chromatic_aberration;
    FilmGrainConfig           film_grain;
};

/// Map an effect name (case-insensitive) to a `PostProcessKind`.
/// Accepts the canonical preset spellings plus common aliases:
///   Bloom, Tonemapping/ToneMapping/Tonemap, Vignette,
///   ColorGrading/LUT/Grade, DoF/DepthOfField, SSAO/AmbientOcclusion,
///   MotionBlur, FXAA/AntiAliasing, ChromaticAberration, FilmGrain/Grain.
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

    /// host_recorded: `RenderPassScheduler::execute_compiled` は本 pass の
    /// Begin/End render pass / pipeline bind を一切発行せず、 PassRecordFn
    /// 内で host が完全に command 記録する。 `PostProcessPipeline::composite`
    /// のように内部で複数 sub-render-pass を持つチェーンを単一 CompiledGraph
    /// entry として表現したいとき使う (Phase 4 step 3)。 当該 pass の
    /// `render_targets` / `attachment_ops` は framebuffer / VkRenderPass 生成
    /// にも使われないので空でよい (CompiledPass.render_pass / framebuffers は
    /// VK_NULL_HANDLE)。
    bool                    host_recorded    = false;
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

// ============================================================
// パイプライン途中編集 API (§12 拡張)
// ============================================================
/// `PipelineProfileDef` の render_passes / post_process_stack を anchor 名
/// 基準で「途中から」 組み替えるための編集操作。 プリセットをコピーして
/// pass を差し込む / 抜く → `PictorRenderer::register_custom_profile()` +
/// `set_profile()` (または active profile を直接編集して
/// `reload_active_profile()`) で実行時にも反映できる — 再 compile は
/// `CompiledPathDriver::recompile()` が面倒を見る。
///
/// post-process 側のチェーン構造 (`PostProcessChain`) の途中編集は
/// `postprocess_chain.h` の `insert_post_process_pass()` 系を使う。

/// `pass_name` を持つ render pass の index。 見つからなければ -1。
int find_render_pass(const PipelineProfileDef& def, std::string_view pass_name);

/// `anchor` の直前 / 直後へ `pass` を挿入する。 anchor 不在または同名 pass
/// が既に存在する場合は false (プロファイルは不変)。
bool insert_render_pass_before(PipelineProfileDef& def, std::string_view anchor,
                               RenderPassDef pass);
bool insert_render_pass_after(PipelineProfileDef& def, std::string_view anchor,
                              RenderPassDef pass);

/// `pass_name` を持つ render pass を取り除く。 見つからなければ false。
bool remove_render_pass(PipelineProfileDef& def, std::string_view pass_name);

/// `name` を持つ post-process エフェクトの index。 見つからなければ -1。
int find_post_process(const PipelineProfileDef& def, std::string_view name);

/// `anchor` の直前 / 直後へ `effect` を挿入する。 `kind` が UNKNOWN なら
/// `post_process_kind_from_name()` で補完する。 anchor 不在または同名
/// エフェクトが既に存在する場合は false。
bool insert_post_process_before(PipelineProfileDef& def, std::string_view anchor,
                                PostProcessDef effect);
bool insert_post_process_after(PipelineProfileDef& def, std::string_view anchor,
                               PostProcessDef effect);

/// `name` を持つ post-process エフェクトを取り除く。 見つからなければ false。
bool remove_post_process(PipelineProfileDef& def, std::string_view name);

/// Pipeline profile manager (§8.4, §12).
/// Provides built-in presets and supports custom profile registration.
class PipelineProfileManager {
public:
    PipelineProfileManager();
    ~PipelineProfileManager();

    /// Register built-in profiles (Low/Mid/High + Lite/Standard/Ultra)
    void register_defaults();

    /// Register a custom profile (§12)
    void register_profile(const PipelineProfileDef& def);

    /// Set active profile by name (§8.4)
    bool set_profile(const std::string& name);

    /// Get current profile
    const PipelineProfileDef& current_profile() const { return profiles_[current_]; }
    const std::string& current_profile_name() const { return profiles_[current_].profile_name; }

    /// Get profile by name
    const PipelineProfileDef* get_profile(const std::string& name) const;

    /// List all registered profile names
    std::vector<std::string> profile_names() const;

    /// Create built-in profiles (§8.1)
    static PipelineProfileDef create_lite_profile();
    static PipelineProfileDef create_standard_profile();
    static PipelineProfileDef create_ultra_profile();

    /// 既定パイプラインの品質 3 段階 (§8.1 拡張)。 途中編集 API で
    /// Low → Mid → High と段階的に組み上がる (= 差分がそのまま仕様):
    ///   - Low : FORWARD 最小構成。 post は Tonemapping のみ。
    ///   - Mid : Low + Shadow/DepthPre pass、 post に Bloom / Vignette。
    ///   - High: Mid + FORWARD_PLUS 相当 (DepthPre → LightCull → shading) +
    ///           SSAO、 post に DepthOfField (深度をチェーンへ捩じ込む)。
    static PipelineProfileDef create_low_profile();
    static PipelineProfileDef create_mid_profile();
    static PipelineProfileDef create_high_profile();

    /// Mobile presets (§8.1 extension).
    /// MobileLow targets bandwidth-bound tile-based GPUs (no shadows, FORWARD, no MSAA).
    /// MobileHigh targets modern mobile SoCs (FORWARD, 1-cascade PCF shadows, light post-process).
    static PipelineProfileDef create_mobile_low_profile();
    static PipelineProfileDef create_mobile_high_profile();

private:
    static constexpr size_t kNoProfile = static_cast<size_t>(-1);

    std::vector<PipelineProfileDef> profiles_;
    // profiles_ への index。 生ポインタだと register_profile() の push_back
    // 再確保で dangling するため index で保持する。
    size_t                          current_ = kNoProfile;
};

} // namespace pictor
