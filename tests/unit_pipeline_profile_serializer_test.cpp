/// PipelineProfileDef <-> JSON round-trip + preset loader fallback +
/// PipelineProfileBuilder structured-pass construction.

#include "pictor/pipeline/pipeline_profile.h"
#include "pictor/pipeline/pipeline_profile_serializer.h"
#include "pictor/pipeline/pipeline_profile_loader.h"
#include "pictor/pipeline/pipeline_builder.h"
#include "pictor/postprocess/postprocess_config_bridge.h"
#include "test_common.h"

#include <cmath>
#include <string>

using namespace pictor;
using namespace pictor_test;

namespace {

bool same_pass(const RenderPassDef& a, const RenderPassDef& b) {
    return a.pass_name        == b.pass_name &&
           a.pass_type        == b.pass_type &&
           a.shader_override  == b.shader_override &&
           a.render_targets   == b.render_targets &&
           a.input_textures   == b.input_textures &&
           a.sort_mode        == b.sort_mode &&
           a.filter_mask      == b.filter_mask &&
           a.gpu_driven_pass  == b.gpu_driven_pass &&
           a.required_streams == b.required_streams;
}

void test_roundtrip_ultra() {
    // Ultra is the richest preset — exercises every pass type, post-process,
    // GI probes, large memory budgets.
    PipelineProfileDef src = PipelineProfileManager::create_ultra_profile();

    std::string json = to_pipeline_profile_json(src);
    PT_ASSERT(!json.empty(), "encoded JSON non-empty");

    PipelineProfileDef dst;
    std::string err;
    bool ok = from_pipeline_profile_json(json, dst, &err);
    PT_ASSERT(ok, err.c_str());

    PT_ASSERT(dst.profile_name == "Ultra", "profile_name round-trips");
    PT_ASSERT(dst.rendering_path == src.rendering_path, "rendering_path round-trips");
    PT_ASSERT_OP(dst.max_lights, ==, src.max_lights, "max_lights round-trips");
    PT_ASSERT_OP(dst.msaa_samples, ==, src.msaa_samples, "msaa_samples round-trips");
    PT_ASSERT(dst.gpu_driven_enabled == src.gpu_driven_enabled, "gpu_driven round-trips");
    PT_ASSERT(dst.compute_update_enabled == src.compute_update_enabled,
              "compute_update round-trips");

    // render_passes
    PT_ASSERT_OP(dst.render_passes.size(), ==, src.render_passes.size(),
                 "render_passes count round-trips");
    bool passes_equal = dst.render_passes.size() == src.render_passes.size();
    for (size_t i = 0; passes_equal && i < src.render_passes.size(); ++i) {
        passes_equal = same_pass(dst.render_passes[i], src.render_passes[i]);
    }
    PT_ASSERT(passes_equal, "every render pass field round-trips");

    // post_process
    PT_ASSERT_OP(dst.post_process_stack.size(), ==, src.post_process_stack.size(),
                 "post_process count round-trips");

    // shadow / gi
    PT_ASSERT_OP(dst.shadow_config.cascade_count, ==, src.shadow_config.cascade_count,
                 "shadow cascade_count round-trips");
    PT_ASSERT(dst.gi_config.gi_probes_enabled == src.gi_config.gi_probes_enabled,
              "gi_probes_enabled round-trips");
    PT_ASSERT_OP(dst.gi_config.probes.grid_x, ==, src.gi_config.probes.grid_x,
                 "gi probe grid_x round-trips");
    PT_ASSERT(dst.gi_config.shadow.filter_mode == src.gi_config.shadow.filter_mode,
              "gi shadow filter_mode round-trips");

    // memory (byte-exact)
    PT_ASSERT_OP(dst.memory_config.frame_allocator_size, ==,
                 src.memory_config.frame_allocator_size, "frame_allocator_size round-trips");
    PT_ASSERT_OP(dst.memory_config.gpu_config.mesh_pool_size, ==,
                 src.memory_config.gpu_config.mesh_pool_size, "mesh_pool_size round-trips");

    // gpu_driven / update / profiler
    PT_ASSERT(dst.gpu_driven_config.two_phase_culling ==
              src.gpu_driven_config.two_phase_culling, "two_phase_culling round-trips");
    PT_ASSERT_OP(dst.update_config.chunk_size, ==, src.update_config.chunk_size,
                 "update chunk_size round-trips");
    PT_ASSERT(dst.profiler_config.overlay_mode == src.profiler_config.overlay_mode,
              "profiler overlay_mode round-trips");
}

void test_preset_seeded_override() {
    // A partial JSON over a Standard preset should override only what it names.
    PipelineProfileDef preset = PipelineProfileManager::create_standard_profile();

    const std::string partial = R"({
      "profile_name": "StandardTweaked",
      "max_lights": 64,
      "msaa_samples": 2
    })";

    PipelineProfileDef out;
    std::string err;
    bool ok = from_pipeline_profile_json(partial, preset, out, &err);
    PT_ASSERT(ok, err.c_str());

    PT_ASSERT(out.profile_name == "StandardTweaked", "name overridden");
    PT_ASSERT_OP(out.max_lights, ==, 64u, "max_lights overridden");
    PT_ASSERT_OP(out.msaa_samples, ==, 2u, "msaa overridden");
    // Untouched fields keep the preset value.
    PT_ASSERT_OP(out.render_passes.size(), ==, preset.render_passes.size(),
                 "render_passes preserved from preset");
    PT_ASSERT_OP(out.shadow_config.cascade_count, ==,
                 preset.shadow_config.cascade_count, "shadow preserved from preset");
}

void test_syntax_error_rejected() {
    PipelineProfileDef out;
    std::string err;
    bool ok = from_pipeline_profile_json("{ \"max_lights\": ", out, &err);
    PT_ASSERT(!ok, "truncated JSON rejected");
    PT_ASSERT(!err.empty(), "error message populated on syntax failure");
}

void test_unknown_keys_ignored() {
    // Forward compatibility: unknown keys must be silently skipped.
    const std::string j = R"({
      "profile_name": "Fwd",
      "future_feature": { "nested": [1, 2, 3] },
      "max_lights": 99
    })";
    PipelineProfileDef out;
    std::string err;
    bool ok = from_pipeline_profile_json(j, out, &err);
    PT_ASSERT(ok, err.c_str());
    PT_ASSERT_OP(out.max_lights, ==, 99u, "known key parsed past unknown key");
}

void test_builder_structured_pass() {
    // PipelineProfileBuilder::make_pass builds a fully-specified pass from
    // string tokens — the structured seam the JSON loader / Ergo-web uses.
    RenderPassDef p = PipelineProfileBuilder::make_pass(
        "GBufferPass", "OPAQUE", "FRONT_TO_BACK",
        {"gbufferA", "gbufferB"}, {"depthTex"}, {"gpu_transforms"},
        0x00FF, true, "handle:42");

    PT_ASSERT(p.pass_name == "GBufferPass", "make_pass name");
    PT_ASSERT(p.pass_type == PassType::OPAQUE, "make_pass type");
    PT_ASSERT(p.sort_mode == SortMode::FRONT_TO_BACK, "make_pass sort_mode");
    PT_ASSERT_OP(p.render_targets.size(), ==, 2u, "make_pass render_targets");
    PT_ASSERT_OP(p.input_textures.size(), ==, 1u, "make_pass input_textures");
    PT_ASSERT_OP(p.required_streams.size(), ==, 1u, "make_pass required_streams");
    PT_ASSERT_OP(p.filter_mask, ==, 0x00FFu, "make_pass filter_mask");
    PT_ASSERT(p.gpu_driven_pass, "make_pass gpu_driven_pass");
    PT_ASSERT_OP(p.shader_override, ==, 42u, "make_pass shader_override handle");

    // Unknown enum strings fall back gracefully.
    RenderPassDef fb = PipelineProfileBuilder::make_pass("X", "NOT_A_TYPE", "BOGUS");
    PT_ASSERT(fb.pass_type == PassType::OPAQUE, "bad pass_type falls back to OPAQUE");
    PT_ASSERT(fb.sort_mode == SortMode::FRONT_TO_BACK,
              "bad sort_mode falls back to FRONT_TO_BACK");

    // "none" shader handle.
    RenderPassDef none = PipelineProfileBuilder::make_pass("Y", "SHADOW", "NONE");
    PT_ASSERT(none.shader_override == INVALID_MESH, "'none' shader -> INVALID_MESH");
}

void test_preset_loader_fallback() {
    // With no profile directory, the loader must fall back to C++ factories
    // and still produce all eight named presets.
    std::vector<PresetLoadResult> results;
    auto profiles = load_builtin_presets("", &results);

    PT_ASSERT_OP(profiles.size(), ==, 8u, "loader returns 8 presets");
    PT_ASSERT_OP(results.size(), ==, 8u, "loader returns 8 results");

    bool any_from_file = false;
    for (const auto& r : results) {
        if (r.loaded_from_file) any_from_file = true;
    }
    PT_ASSERT(!any_from_file, "empty dir -> all presets from C++ fallback");

    // Names must match the C++ factory output exactly.
    PT_ASSERT(profiles[0].profile_name == "Low",        "preset[0] = Low");
    PT_ASSERT(profiles[1].profile_name == "Mid",        "preset[1] = Mid");
    PT_ASSERT(profiles[2].profile_name == "High",       "preset[2] = High");
    PT_ASSERT(profiles[3].profile_name == "Lite",       "preset[3] = Lite");
    PT_ASSERT(profiles[4].profile_name == "Standard",   "preset[4] = Standard");
    PT_ASSERT(profiles[5].profile_name == "Ultra",      "preset[5] = Ultra");
    PT_ASSERT(profiles[6].profile_name == "MobileLow",  "preset[6] = MobileLow");
    PT_ASSERT(profiles[7].profile_name == "MobileHigh", "preset[7] = MobileHigh");

    // Fallback content must equal the C++ factory directly.
    PipelineProfileDef std_cpp = PipelineProfileManager::create_standard_profile();
    PT_ASSERT_OP(profiles[4].render_passes.size(), ==, std_cpp.render_passes.size(),
                 "fallback Standard matches C++ factory pass count");
}

void test_file_io() {
    // Write a profile, read it back, compare a few fields.
    PipelineProfileDef src = PipelineProfileManager::create_mobile_high_profile();
    const std::string path = "test_pipeline_profile_io.json";

    std::string err;
    bool wrote = save_pipeline_profile_file(path, src, &err);
    PT_ASSERT(wrote, err.c_str());

    PipelineProfileDef loaded;
    bool read = load_pipeline_profile_file(path, loaded, &err);
    PT_ASSERT(read, err.c_str());
    PT_ASSERT(loaded.profile_name == "MobileHigh", "file I/O preserves profile_name");
    PT_ASSERT_OP(loaded.max_lights, ==, src.max_lights, "file I/O preserves max_lights");

    std::remove(path.c_str());

    // Missing file -> graceful failure.
    PipelineProfileDef missing;
    bool ok = load_pipeline_profile_file("definitely_not_here.json", missing, &err);
    PT_ASSERT(!ok, "missing file load fails cleanly");
    PT_ASSERT(!err.empty(), "missing file populates error");
}

bool near_eq(float a, float b) { return std::fabs(a - b) < 1e-4f; }

void test_post_process_param_roundtrip() {
    // A profile with a typed post-process stack must round-trip every
    // effect parameter, not just name/enabled (方針2 phase 1).
    PipelineProfileDef src;
    src.profile_name = "PPParams";

    PostProcessDef bloom;
    bloom.name = "Bloom";
    bloom.kind = PostProcessKind::BLOOM;
    bloom.enabled = true;
    bloom.bloom.threshold      = 0.62f;
    bloom.bloom.soft_threshold = 0.33f;
    bloom.bloom.intensity      = 0.91f;
    bloom.bloom.radius         = 7.5f;
    bloom.bloom.mip_levels     = 6;
    bloom.bloom.scatter        = 0.44f;

    PostProcessDef tm;
    tm.name = "ToneMapping";
    tm.kind = PostProcessKind::TONE_MAPPING;
    tm.enabled = false;
    tm.tone_mapping.op         = ToneMapOperator::UNCHARTED2;
    tm.tone_mapping.exposure   = 1.3f;
    tm.tone_mapping.saturation = 1.1f;

    PostProcessDef vig;
    vig.name = "Vignette";
    vig.kind = PostProcessKind::VIGNETTE;
    vig.vignette.intensity = 0.5f;
    vig.vignette.radius    = 0.6f;
    vig.vignette.color[0]  = 0.1f;
    vig.vignette.color[1]  = 0.2f;
    vig.vignette.color[2]  = 0.3f;

    PostProcessDef grade;
    grade.name = "ColorGrading";
    grade.kind = PostProcessKind::COLOR_GRADING;
    grade.color_grading.lut_path      = "data/lut/warm.png";
    grade.color_grading.lut_intensity = 0.8f;
    grade.color_grading.lut_size      = 32;

    PostProcessDef dof;
    dof.name = "DoF";
    dof.kind = PostProcessKind::DEPTH_OF_FIELD;
    dof.depth_of_field.focus_distance = 12.5f;
    dof.depth_of_field.bokeh_radius   = 5.0f;
    dof.depth_of_field.sample_count   = 24;

    // SSAO has no host-driven implementation — kind stays UNKNOWN.
    PostProcessDef ssao;
    ssao.name = "SSAO";

    src.post_process_stack = {bloom, tm, vig, grade, dof, ssao};

    std::string json = to_pipeline_profile_json(src);
    PipelineProfileDef dst;
    std::string err;
    PT_ASSERT(from_pipeline_profile_json(json, dst, &err), err.c_str());

    PT_ASSERT_OP(dst.post_process_stack.size(), ==, 6u, "post_process count round-trips");

    const auto& b = dst.post_process_stack[0];
    PT_ASSERT(b.kind == PostProcessKind::BLOOM, "bloom kind round-trips");
    PT_ASSERT(b.enabled, "bloom enabled round-trips");
    PT_ASSERT(near_eq(b.bloom.threshold, 0.62f), "bloom threshold round-trips");
    PT_ASSERT(near_eq(b.bloom.intensity, 0.91f), "bloom intensity round-trips");
    PT_ASSERT_OP(b.bloom.mip_levels, ==, 6u, "bloom mip_levels round-trips");

    const auto& t = dst.post_process_stack[1];
    PT_ASSERT(t.kind == PostProcessKind::TONE_MAPPING, "tonemap kind round-trips");
    PT_ASSERT(!t.enabled, "tonemap disabled round-trips");
    PT_ASSERT(t.tone_mapping.op == ToneMapOperator::UNCHARTED2,
              "tonemap operator round-trips");
    PT_ASSERT(near_eq(t.tone_mapping.exposure, 1.3f), "tonemap exposure round-trips");

    const auto& v = dst.post_process_stack[2];
    PT_ASSERT(near_eq(v.vignette.intensity, 0.5f), "vignette intensity round-trips");
    PT_ASSERT(near_eq(v.vignette.color[2], 0.3f), "vignette color round-trips");

    const auto& g = dst.post_process_stack[3];
    PT_ASSERT(g.color_grading.lut_path == "data/lut/warm.png", "LUT path round-trips");
    PT_ASSERT_OP(g.color_grading.lut_size, ==, 32u, "LUT size round-trips");

    const auto& d = dst.post_process_stack[4];
    PT_ASSERT(near_eq(d.depth_of_field.focus_distance, 12.5f),
              "DoF focus_distance round-trips");
    PT_ASSERT_OP(d.depth_of_field.sample_count, ==, 24u, "DoF sample_count round-trips");

    // SSAO: kind inferred as UNKNOWN, no host implementation.
    PT_ASSERT(dst.post_process_stack[5].kind == PostProcessKind::UNKNOWN,
              "SSAO resolves to UNKNOWN kind");
}

void test_post_process_kind_inference() {
    // A bare { "name": ... } with no explicit "kind" must still resolve to
    // the right kind on parse (preset spelling compatibility).
    const std::string j = R"({
      "profile_name": "Inferred",
      "post_process": [
        { "name": "Bloom",       "enabled": true },
        { "name": "Tonemapping", "enabled": true },
        { "name": "Grade",       "enabled": true },
        { "name": "FXAA",        "enabled": true }
      ]
    })";
    PipelineProfileDef out;
    std::string err;
    PT_ASSERT(from_pipeline_profile_json(j, out, &err), err.c_str());
    PT_ASSERT_OP(out.post_process_stack.size(), ==, 4u, "stack parsed");
    PT_ASSERT(out.post_process_stack[0].kind == PostProcessKind::BLOOM,
              "name 'Bloom' infers BLOOM");
    PT_ASSERT(out.post_process_stack[1].kind == PostProcessKind::TONE_MAPPING,
              "name 'Tonemapping' infers TONE_MAPPING");
    PT_ASSERT(out.post_process_stack[2].kind == PostProcessKind::COLOR_GRADING,
              "name 'Grade' infers COLOR_GRADING");
    PT_ASSERT(out.post_process_stack[3].kind == PostProcessKind::UNKNOWN,
              "name 'FXAA' infers UNKNOWN");
}

void test_post_process_bridge() {
    // build_post_process_config folds a stack into a PostProcessConfig.
    PipelineProfileDef profile;

    PostProcessDef bloom;
    bloom.name = "Bloom";
    bloom.kind = PostProcessKind::BLOOM;
    bloom.enabled = true;
    bloom.bloom.intensity = 0.77f;

    PostProcessDef vig;
    vig.name = "Vignette";
    vig.kind = PostProcessKind::VIGNETTE;
    vig.enabled = false;            // present but disabled
    vig.vignette.intensity = 0.9f;

    PostProcessDef ssao;            // UNKNOWN — must be ignored
    ssao.name = "SSAO";

    profile.post_process_stack = {bloom, vig, ssao};

    PostProcessConfig cfg = build_post_process_config(profile);

    // Bloom: enabled with the def's parameter.
    PT_ASSERT(cfg.bloom.enabled, "bridge enables Bloom from stack");
    PT_ASSERT(near_eq(cfg.bloom.intensity, 0.77f), "bridge applies bloom intensity");

    // Vignette present-but-disabled: parameters copied, enabled=false.
    PT_ASSERT(!cfg.vignette.enabled, "bridge keeps Vignette disabled");
    PT_ASSERT(near_eq(cfg.vignette.intensity, 0.9f),
              "bridge still applies disabled-effect params");

    // Tone-mapping NOT in the stack -> forced off.
    PT_ASSERT(!cfg.tone_mapping.enabled, "effect absent from stack is disabled");
    PT_ASSERT(!cfg.color_grading.enabled, "absent color_grading disabled");
    PT_ASSERT(!cfg.depth_of_field.enabled, "absent depth_of_field disabled");

    // `base` HDR / gaussian_blur survive (no PostProcessDef mapping).
    PostProcessConfig base;
    base.hdr.exposure = 2.5f;
    base.gaussian_blur.enabled = true;
    PostProcessConfig merged = build_post_process_config(profile, base);
    PT_ASSERT(near_eq(merged.hdr.exposure, 2.5f), "bridge preserves base HDR");
    PT_ASSERT(merged.gaussian_blur.enabled, "bridge preserves base gaussian_blur");
}

} // namespace

int main() {
    test_roundtrip_ultra();
    test_preset_seeded_override();
    test_syntax_error_rejected();
    test_unknown_keys_ignored();
    test_builder_structured_pass();
    test_preset_loader_fallback();
    test_file_io();
    test_post_process_param_roundtrip();
    test_post_process_kind_inference();
    test_post_process_bridge();
    return report("unit_pipeline_profile_serializer_test");
}
