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

private:
    PipelineProfileDef def_{};
};

} // namespace pictor
