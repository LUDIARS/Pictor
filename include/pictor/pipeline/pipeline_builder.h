#pragma once

#include "pictor/pipeline/pipeline_profile.h"
#include <string>
#include <string_view>
#include <unordered_map>

namespace pictor {

/// Fluent builder for constructing PipelineProfileDef from external code
/// without needing to touch PipelineProfileManager internals.
///
/// Usage (programmatic):
///
///     auto def = PipelineProfileBuilder("MyMobile")
///                    .with_rendering_path(RenderingPath::FORWARD)
///                    .with_max_lights(16)
///                    .with_shadow(1, 1024, ShadowFilterMode::PCF)
///                    .with_msaa(2)
///                    .with_memory_budget_mb(2, 64, 16)
///                    .add_pass({"OpaquePass", PassType::OPAQUE})
///                    .add_pass({"TransparentPass", PassType::TRANSPARENT,
///                               INVALID_MESH, {}, {}, SortMode::BACK_TO_FRONT})
///                    .add_post_process("Tonemapping")
///                    .build();
///     renderer.register_custom_profile(def);
///
/// Usage (data-driven, e.g. loaded from JSON/TOML by the host application):
///
///     std::unordered_map<std::string, std::string> cfg = { ... };
///     auto def = PipelineProfileBuilder::from_key_value(cfg);
///
/// The builder performs no I/O and has no external dependencies — the host
/// application owns parsing (JSON, YAML, INI, etc.) and passes the resulting
/// key/value pairs in.
class PipelineProfileBuilder {
public:
    explicit PipelineProfileBuilder(std::string profile_name);

    /// Seed the builder from an existing preset (e.g. one of the MobileLow /
    /// MobileHigh factories) and then customize further.
    static PipelineProfileBuilder from_preset(const PipelineProfileDef& preset);

    // ---- Scalar / enum settings --------------------------------------------

    PipelineProfileBuilder& with_rendering_path(RenderingPath path);
    PipelineProfileBuilder& with_max_lights(uint32_t count);
    PipelineProfileBuilder& with_msaa(uint8_t samples);
    PipelineProfileBuilder& with_gpu_driven(bool enabled);
    PipelineProfileBuilder& with_compute_update(bool enabled);

    // ---- Shadows -----------------------------------------------------------

    /// Configure CSM shadows. Pass cascade_count=0 to disable.
    PipelineProfileBuilder& with_shadow(uint32_t cascade_count,
                                        uint32_t resolution,
                                        ShadowFilterMode filter_mode);

    // ---- Memory budget -----------------------------------------------------

    /// Quick helper: set the three dominant budgets in MB.
    /// frame_alloc_mb covers per-frame scratch, mesh_pool_mb is the vertex/
    /// index arena, ssbo_pool_mb covers draw/instance SSBOs.
    PipelineProfileBuilder& with_memory_budget_mb(size_t frame_alloc_mb,
                                                   size_t mesh_pool_mb,
                                                   size_t ssbo_pool_mb);

    PipelineProfileBuilder& with_flight_count(uint32_t count);

    // ---- Render passes -----------------------------------------------------

    /// Replace the entire pass list.
    PipelineProfileBuilder& with_passes(std::vector<RenderPassDef> passes);

    /// Append a single pass.
    PipelineProfileBuilder& add_pass(RenderPassDef pass);

    /// Remove a pass by name. No-op if not found.
    PipelineProfileBuilder& remove_pass(std::string_view pass_name);

    // ---- Post-process stack ------------------------------------------------

    PipelineProfileBuilder& with_post_process(std::vector<PostProcessDef> stack);
    PipelineProfileBuilder& add_post_process(std::string name, bool enabled = true);
    PipelineProfileBuilder& clear_post_process();

    // ---- Profiler ----------------------------------------------------------

    PipelineProfileBuilder& with_profiler(bool enabled, OverlayMode mode);

    // ---- GI ----------------------------------------------------------------

    PipelineProfileBuilder& with_gi_config(const GIConfig& cfg);

    // ---- Build -------------------------------------------------------------

    /// Returns the assembled definition. The builder remains usable afterward.
    const PipelineProfileDef& build() const { return def_; }
    PipelineProfileDef take() { return std::move(def_); }

    // ---- Data-driven construction -----------------------------------------

    /// Construct a profile from a flat key/value string map.
    ///
    /// Recognized keys (all optional except name):
    ///   name                     (string, required)
    ///   rendering_path           ("FORWARD" | "FORWARD_PLUS" | "DEFERRED" | "HYBRID")
    ///   max_lights               (uint)
    ///   msaa_samples             (uint, 0/2/4/8)
    ///   gpu_driven               ("true" | "false")
    ///   compute_update           ("true" | "false")
    ///   shadow.cascade_count     (uint; 0 disables shadows)
    ///   shadow.resolution        (uint)
    ///   shadow.filter_mode       ("NONE" | "PCF" | "PCSS")
    ///   memory.frame_alloc_mb    (uint)
    ///   memory.mesh_pool_mb      (uint)
    ///   memory.ssbo_pool_mb      (uint)
    ///   memory.flight_count      (uint)
    ///   post_process             (comma-separated list, e.g. "Bloom,Tonemapping,FXAA")
    ///   passes                   (comma-separated pass names; pass types default to OPAQUE)
    ///   profiler.enabled         ("true" | "false")
    ///   profiler.overlay         ("OFF" | "MINIMAL" | "STANDARD" | "DETAILED" | "TIMELINE")
    ///
    /// Unknown keys are ignored (forward compatibility). Invalid values fall
    /// back to the preset defaults so the output is always a usable profile.
    static PipelineProfileDef from_key_value(
        const std::unordered_map<std::string, std::string>& kv);

    /// Same as from_key_value() but seeded from an existing preset, so the
    /// caller only needs to specify overrides.
    static PipelineProfileDef from_key_value(
        const PipelineProfileDef& preset,
        const std::unordered_map<std::string, std::string>& kv);

    // ---- Structured render-pass construction -------------------------------
    //
    // flat-KV from_key_value() can only express pass *names* (all forced to
    // OPAQUE). These helpers let a structured loader (e.g. the JSON serializer
    // in pipeline_profile_serializer.h) build a fully-specified RenderPassDef
    // from string/enum tokens without touching enum headers directly, and is
    // the seam the Ergo-web pipeline editor drives.

    /// Build a RenderPassDef from string tokens. Unknown enum strings fall
    /// back to the supplied default. `shader_override` accepts "none" or
    /// "handle:<u32>". This performs no I/O.
    static RenderPassDef make_pass(
        const std::string&              pass_name,
        const std::string&              pass_type_str,      // see PassType
        const std::string&              sort_mode_str    = "FRONT_TO_BACK",
        const std::vector<std::string>& render_targets   = {},
        const std::vector<std::string>& input_textures   = {},
        const std::vector<std::string>& required_streams = {},
        uint16_t                        filter_mask      = 0xFFFF,
        bool                            gpu_driven_pass  = false,
        const std::string&              shader_override  = "none");

    /// Append a fully-specified pass built from string tokens.
    PipelineProfileBuilder& add_pass_tokens(
        const std::string&              pass_name,
        const std::string&              pass_type_str,
        const std::string&              sort_mode_str    = "FRONT_TO_BACK",
        const std::vector<std::string>& render_targets   = {},
        const std::vector<std::string>& input_textures   = {},
        const std::vector<std::string>& required_streams = {},
        uint16_t                        filter_mask      = 0xFFFF,
        bool                            gpu_driven_pass  = false,
        const std::string&              shader_override  = "none");

private:
    PipelineProfileDef def_{};
};

} // namespace pictor
