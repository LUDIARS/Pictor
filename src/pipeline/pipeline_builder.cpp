#include "pictor/pipeline/pipeline_builder.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace pictor {

namespace {

// ---- small parsing helpers -------------------------------------------------

std::string to_upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool parse_bool(const std::string& v, bool fallback) {
    std::string u = to_upper(v);
    if (u == "TRUE" || u == "1" || u == "ON" || u == "YES")  return true;
    if (u == "FALSE" || u == "0" || u == "OFF" || u == "NO") return false;
    return fallback;
}

uint32_t parse_uint(const std::string& v, uint32_t fallback) {
    if (v.empty()) return fallback;
    char* end = nullptr;
    unsigned long n = std::strtoul(v.c_str(), &end, 10);
    if (end == v.c_str()) return fallback;
    return static_cast<uint32_t>(n);
}

RenderingPath parse_rendering_path(const std::string& v, RenderingPath fallback) {
    std::string u = to_upper(v);
    if (u == "FORWARD")      return RenderingPath::FORWARD;
    if (u == "FORWARD_PLUS") return RenderingPath::FORWARD_PLUS;
    if (u == "DEFERRED")     return RenderingPath::DEFERRED;
    if (u == "HYBRID")       return RenderingPath::HYBRID;
    return fallback;
}

ShadowFilterMode parse_shadow_filter(const std::string& v, ShadowFilterMode fallback) {
    std::string u = to_upper(v);
    if (u == "NONE") return ShadowFilterMode::NONE;
    if (u == "PCF")  return ShadowFilterMode::PCF;
    if (u == "PCSS") return ShadowFilterMode::PCSS;
    return fallback;
}

OverlayMode parse_overlay(const std::string& v, OverlayMode fallback) {
    std::string u = to_upper(v);
    if (u == "OFF")      return OverlayMode::OFF;
    if (u == "MINIMAL")  return OverlayMode::MINIMAL;
    if (u == "STANDARD") return OverlayMode::STANDARD;
    if (u == "DETAILED") return OverlayMode::DETAILED;
    if (u == "TIMELINE") return OverlayMode::TIMELINE;
    return fallback;
}

std::vector<std::string> split_csv(const std::string& v) {
    std::vector<std::string> out;
    std::stringstream ss(v);
    std::string token;
    while (std::getline(ss, token, ',')) {
        auto b = token.find_first_not_of(" \t");
        auto e = token.find_last_not_of(" \t");
        if (b == std::string::npos) continue;
        out.emplace_back(token.substr(b, e - b + 1));
    }
    return out;
}

const std::string* find(const std::unordered_map<std::string, std::string>& kv,
                        const char* key) {
    auto it = kv.find(key);
    return (it != kv.end()) ? &it->second : nullptr;
}

} // namespace

// ---- ctors -----------------------------------------------------------------

PipelineProfileBuilder::PipelineProfileBuilder(std::string profile_name) {
    def_.profile_name = std::move(profile_name);
}

PipelineProfileBuilder PipelineProfileBuilder::from_preset(const PipelineProfileDef& preset) {
    PipelineProfileBuilder b(preset.profile_name);
    b.def_ = preset;
    return b;
}

// ---- scalar setters --------------------------------------------------------

PipelineProfileBuilder& PipelineProfileBuilder::with_rendering_path(RenderingPath path) {
    def_.rendering_path = path;
    return *this;
}

PipelineProfileBuilder& PipelineProfileBuilder::with_max_lights(uint32_t count) {
    def_.max_lights = count;
    return *this;
}

PipelineProfileBuilder& PipelineProfileBuilder::with_msaa(uint8_t samples) {
    def_.msaa_samples = samples;
    return *this;
}

PipelineProfileBuilder& PipelineProfileBuilder::with_gpu_driven(bool enabled) {
    def_.gpu_driven_enabled = enabled;
    return *this;
}

PipelineProfileBuilder& PipelineProfileBuilder::with_compute_update(bool enabled) {
    def_.compute_update_enabled = enabled;
    def_.gpu_driven_config.compute_update = enabled;
    return *this;
}

PipelineProfileBuilder& PipelineProfileBuilder::with_shadow(uint32_t cascade_count,
                                                             uint32_t resolution,
                                                             ShadowFilterMode filter_mode) {
    def_.shadow_config.cascade_count = cascade_count;
    def_.shadow_config.resolution    = resolution;
    def_.shadow_config.filter_mode   = filter_mode;

    def_.gi_config.shadow_enabled        = (cascade_count > 0);
    def_.gi_config.shadow.cascade_count  = cascade_count;
    def_.gi_config.shadow.resolution     = resolution;
    def_.gi_config.shadow.filter_mode    = filter_mode;
    return *this;
}

PipelineProfileBuilder& PipelineProfileBuilder::with_memory_budget_mb(size_t frame_alloc_mb,
                                                                       size_t mesh_pool_mb,
                                                                       size_t ssbo_pool_mb) {
    constexpr size_t MB = 1024 * 1024;
    def_.memory_config.frame_allocator_size         = frame_alloc_mb * MB;
    def_.memory_config.gpu_config.mesh_pool_size    = mesh_pool_mb   * MB;
    def_.memory_config.gpu_config.ssbo_pool_size    = ssbo_pool_mb   * MB;
    return *this;
}

PipelineProfileBuilder& PipelineProfileBuilder::with_flight_count(uint32_t count) {
    def_.memory_config.flight_count = count;
    return *this;
}

// ---- passes ---------------------------------------------------------------

PipelineProfileBuilder& PipelineProfileBuilder::with_passes(std::vector<RenderPassDef> passes) {
    def_.render_passes = std::move(passes);
    return *this;
}

PipelineProfileBuilder& PipelineProfileBuilder::add_pass(RenderPassDef pass) {
    def_.render_passes.push_back(std::move(pass));
    return *this;
}

PipelineProfileBuilder& PipelineProfileBuilder::remove_pass(std::string_view pass_name) {
    auto& v = def_.render_passes;
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](const RenderPassDef& p) { return p.pass_name == pass_name; }),
            v.end());
    return *this;
}

// ---- post-process ---------------------------------------------------------

PipelineProfileBuilder& PipelineProfileBuilder::with_post_process(std::vector<PostProcessDef> stack) {
    def_.post_process_stack = std::move(stack);
    return *this;
}

PipelineProfileBuilder& PipelineProfileBuilder::add_post_process(std::string name, bool enabled) {
    def_.post_process_stack.push_back({std::move(name), enabled});
    return *this;
}

PipelineProfileBuilder& PipelineProfileBuilder::clear_post_process() {
    def_.post_process_stack.clear();
    return *this;
}

// ---- profiler / GI --------------------------------------------------------

PipelineProfileBuilder& PipelineProfileBuilder::with_profiler(bool enabled, OverlayMode mode) {
    def_.profiler_config.enabled      = enabled;
    def_.profiler_config.overlay_mode = mode;
    return *this;
}

PipelineProfileBuilder& PipelineProfileBuilder::with_gi_config(const GIConfig& cfg) {
    def_.gi_config = cfg;
    return *this;
}

// ---- data-driven construction ---------------------------------------------

PipelineProfileDef PipelineProfileBuilder::from_key_value(
    const std::unordered_map<std::string, std::string>& kv) {

    const std::string* name_v = find(kv, "name");
    PipelineProfileBuilder b(name_v ? *name_v : std::string("Custom"));
    // Seed with a sensible default (Lite) so minimal configs still produce a
    // runnable profile.
    auto seed = PipelineProfileManager::create_lite_profile();
    seed.profile_name = b.def_.profile_name;
    b.def_ = seed;

    return from_key_value(b.def_, kv);
}

PipelineProfileDef PipelineProfileBuilder::from_key_value(
    const PipelineProfileDef& preset,
    const std::unordered_map<std::string, std::string>& kv) {

    PipelineProfileBuilder b = PipelineProfileBuilder::from_preset(preset);

    if (auto* v = find(kv, "name"))              b.def_.profile_name = *v;
    if (auto* v = find(kv, "rendering_path"))    b.with_rendering_path(parse_rendering_path(*v, preset.rendering_path));
    if (auto* v = find(kv, "max_lights"))        b.with_max_lights(parse_uint(*v, preset.max_lights));
    if (auto* v = find(kv, "msaa_samples"))      b.with_msaa(static_cast<uint8_t>(parse_uint(*v, preset.msaa_samples)));
    if (auto* v = find(kv, "gpu_driven"))        b.with_gpu_driven(parse_bool(*v, preset.gpu_driven_enabled));
    if (auto* v = find(kv, "compute_update"))    b.with_compute_update(parse_bool(*v, preset.compute_update_enabled));

    // Shadows — only re-apply if any shadow.* key is present so that presets
    // with shadow configs remain intact when caller doesn't override.
    const bool has_shadow_key =
        find(kv, "shadow.cascade_count") || find(kv, "shadow.resolution") ||
        find(kv, "shadow.filter_mode");
    if (has_shadow_key) {
        uint32_t cc   = parse_uint(find(kv, "shadow.cascade_count") ? *find(kv, "shadow.cascade_count") : "",
                                   preset.shadow_config.cascade_count);
        uint32_t res  = parse_uint(find(kv, "shadow.resolution") ? *find(kv, "shadow.resolution") : "",
                                   preset.shadow_config.resolution);
        ShadowFilterMode fm = parse_shadow_filter(
            find(kv, "shadow.filter_mode") ? *find(kv, "shadow.filter_mode") : "",
            preset.shadow_config.filter_mode);
        b.with_shadow(cc, res, fm);
    }

    // Memory
    const bool has_mem_key =
        find(kv, "memory.frame_alloc_mb") ||
        find(kv, "memory.mesh_pool_mb") ||
        find(kv, "memory.ssbo_pool_mb");
    if (has_mem_key) {
        constexpr size_t MB = 1024 * 1024;
        size_t fa   = parse_uint(find(kv, "memory.frame_alloc_mb") ? *find(kv, "memory.frame_alloc_mb") : "",
                                 static_cast<uint32_t>(preset.memory_config.frame_allocator_size / MB));
        size_t mesh = parse_uint(find(kv, "memory.mesh_pool_mb") ? *find(kv, "memory.mesh_pool_mb") : "",
                                 static_cast<uint32_t>(preset.memory_config.gpu_config.mesh_pool_size / MB));
        size_t ssbo = parse_uint(find(kv, "memory.ssbo_pool_mb") ? *find(kv, "memory.ssbo_pool_mb") : "",
                                 static_cast<uint32_t>(preset.memory_config.gpu_config.ssbo_pool_size / MB));
        b.with_memory_budget_mb(fa, mesh, ssbo);
    }
    if (auto* v = find(kv, "memory.flight_count")) {
        b.with_flight_count(parse_uint(*v, preset.memory_config.flight_count));
    }

    // Post-process: replace the whole stack if specified.
    if (auto* v = find(kv, "post_process")) {
        b.clear_post_process();
        for (auto& name : split_csv(*v)) {
            b.add_post_process(std::move(name), true);
        }
    }

    // Passes: replace pass list with simple default-OPAQUE entries. Host apps
    // needing finer-grained control should use the programmatic API.
    if (auto* v = find(kv, "passes")) {
        std::vector<RenderPassDef> passes;
        for (auto& pname : split_csv(*v)) {
            RenderPassDef p;
            p.pass_name = pname;
            p.pass_type = PassType::OPAQUE;
            p.sort_mode = SortMode::FRONT_TO_BACK;
            passes.push_back(std::move(p));
        }
        b.with_passes(std::move(passes));
    }

    // Profiler
    if (auto* v = find(kv, "profiler.enabled")) {
        OverlayMode m = preset.profiler_config.overlay_mode;
        if (auto* ov = find(kv, "profiler.overlay")) {
            m = parse_overlay(*ov, m);
        }
        b.with_profiler(parse_bool(*v, preset.profiler_config.enabled), m);
    } else if (auto* v = find(kv, "profiler.overlay")) {
        b.with_profiler(preset.profiler_config.enabled, parse_overlay(*v, preset.profiler_config.overlay_mode));
    }

    return b.take();
}

} // namespace pictor
