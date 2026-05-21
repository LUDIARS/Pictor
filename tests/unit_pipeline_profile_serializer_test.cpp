/// PipelineProfileDef <-> JSON round-trip + preset loader fallback +
/// PipelineProfileBuilder structured-pass construction.

#include "pictor/pipeline/pipeline_profile.h"
#include "pictor/pipeline/pipeline_profile_serializer.h"
#include "pictor/pipeline/pipeline_profile_loader.h"
#include "pictor/pipeline/pipeline_builder.h"
#include "test_common.h"

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
    // and still produce all five named presets.
    std::vector<PresetLoadResult> results;
    auto profiles = load_builtin_presets("", &results);

    PT_ASSERT_OP(profiles.size(), ==, 5u, "loader returns 5 presets");
    PT_ASSERT_OP(results.size(), ==, 5u, "loader returns 5 results");

    bool any_from_file = false;
    for (const auto& r : results) {
        if (r.loaded_from_file) any_from_file = true;
    }
    PT_ASSERT(!any_from_file, "empty dir -> all presets from C++ fallback");

    // Names must match the C++ factory output exactly.
    PT_ASSERT(profiles[0].profile_name == "Lite",       "preset[0] = Lite");
    PT_ASSERT(profiles[1].profile_name == "Standard",   "preset[1] = Standard");
    PT_ASSERT(profiles[2].profile_name == "Ultra",      "preset[2] = Ultra");
    PT_ASSERT(profiles[3].profile_name == "MobileLow",  "preset[3] = MobileLow");
    PT_ASSERT(profiles[4].profile_name == "MobileHigh", "preset[4] = MobileHigh");

    // Fallback content must equal the C++ factory directly.
    PipelineProfileDef std_cpp = PipelineProfileManager::create_standard_profile();
    PT_ASSERT_OP(profiles[1].render_passes.size(), ==, std_cpp.render_passes.size(),
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

} // namespace

int main() {
    test_roundtrip_ultra();
    test_preset_seeded_override();
    test_syntax_error_rejected();
    test_unknown_keys_ignored();
    test_builder_structured_pass();
    test_preset_loader_fallback();
    test_file_io();
    return report("unit_pipeline_profile_serializer_test");
}
