/// Pictor Mobile Rendering Pipeline Demo
///
/// Demonstrates:
///  1. Two built-in mobile pipeline profiles (MobileLow / MobileHigh) tuned
///     for tile-based GPUs (Adreno, Mali, Apple A-series).
///  2. External / data-driven pipeline construction via PipelineProfileBuilder,
///     including JSON-style key/value configuration the host app would load
///     from disk at runtime.
///  3. Live profile switching at runtime — the scheduler and memory subsystem
///     reconfigure without destroying the scene graph.
///
/// Runs headless (no Vulkan / GLFW dependency) so it builds and executes on
/// CI, docker images, and mobile emulators. If a Vulkan SDK is present the
/// demo still only drives the data pipeline — the heavy GPU submission path
/// lives in pictor_demo / pictor_benchmark.
///
/// Command-line flags:
///   --profile=<name>   Start profile (default: MobileLow)
///   --frames=<N>       Number of frames to simulate per profile (default: 120)
///   --objects=<N>      Scene object count (default: 2000 for low / 20000 for high)
///   --switch           Cycle through all mobile profiles, N frames each
///   --config=<path>    Load a key=value profile config (see kv_profile.ini format)

#include "pictor/pictor.h"
#include "pictor/pipeline/pipeline_builder.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace pictor;

namespace {

// ---- Utility: scene populator (lissajous-moving cubes) ---------------------

class MobileUpdateCallback : public IUpdateCallback {
public:
    float total_time = 0.0f;

    void update(float4x4* transforms, AABB* bounds,
                uint32_t start, uint32_t count, float delta_time) override {
        total_time += delta_time;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t idx = start + i;
            float phase = static_cast<float>(idx) * 0.17f;
            float x = std::sin(total_time * 0.8f + phase) * 6.0f;
            float y = std::cos(total_time * 1.3f + phase) * 3.0f;
            float z = std::sin(total_time * 0.5f + phase * 2.1f) * 6.0f;

            transforms[idx] = float4x4::identity();
            transforms[idx].set_translation(x, y, z);
            bounds[idx].min = {x - 0.5f, y - 0.5f, z - 0.5f};
            bounds[idx].max = {x + 0.5f, y + 0.5f, z + 0.5f};
        }
    }
};

void register_scene(PictorRenderer& renderer, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        ObjectDescriptor desc;
        desc.mesh        = 0;
        desc.material    = 0;
        desc.transform   = float4x4::identity();
        desc.bounds.min  = {-0.5f, -0.5f, -0.5f};
        desc.bounds.max  = { 0.5f,  0.5f,  0.5f};
        desc.shaderKey   = i & 0x3; // four shader-key buckets
        desc.materialKey = 0;
        desc.flags       = ObjectFlags::DYNAMIC | ObjectFlags::INSTANCED |
                           ObjectFlags::CAST_SHADOW | ObjectFlags::RECEIVE_SHADOW;
        renderer.register_object(desc);
    }
}

Camera make_camera() {
    Camera cam;
    cam.position = {0.0f, 2.0f, 15.0f};
    for (int i = 0; i < 6; ++i) {
        cam.frustum.planes[i].normal   = {0, 0, 0};
        cam.frustum.planes[i].distance = 1000.0f;
    }
    cam.frustum.planes[0] = {{ 1, 0, 0}, 50.0f};
    cam.frustum.planes[1] = {{-1, 0, 0}, 50.0f};
    cam.frustum.planes[2] = {{ 0, 1, 0}, 50.0f};
    cam.frustum.planes[3] = {{ 0,-1, 0}, 50.0f};
    cam.frustum.planes[4] = {{ 0, 0, 1}, 0.1f};
    cam.frustum.planes[5] = {{ 0, 0,-1}, 200.0f};
    return cam;
}

// ---- Pretty-printing a profile definition ---------------------------------

const char* rp_name(RenderingPath p) {
    switch (p) {
        case RenderingPath::FORWARD:      return "FORWARD";
        case RenderingPath::FORWARD_PLUS: return "FORWARD_PLUS";
        case RenderingPath::DEFERRED:     return "DEFERRED";
        case RenderingPath::HYBRID:       return "HYBRID";
    }
    return "?";
}

const char* shadow_name(ShadowFilterMode m) {
    switch (m) {
        case ShadowFilterMode::NONE: return "NONE";
        case ShadowFilterMode::PCF:  return "PCF";
        case ShadowFilterMode::PCSS: return "PCSS";
    }
    return "?";
}

void print_profile(const PipelineProfileDef& p) {
    constexpr size_t MB = 1024 * 1024;
    printf("  Profile '%s'\n", p.profile_name.c_str());
    printf("    rendering_path  : %s\n", rp_name(p.rendering_path));
    printf("    max_lights      : %u\n", p.max_lights);
    printf("    msaa_samples    : %u\n", static_cast<unsigned>(p.msaa_samples));
    printf("    gpu_driven      : %s\n", p.gpu_driven_enabled ? "on" : "off");
    printf("    compute_update  : %s\n", p.compute_update_enabled ? "on" : "off");
    printf("    shadows         : %u cascade(s), %ux, filter=%s\n",
           p.shadow_config.cascade_count,
           p.shadow_config.resolution,
           shadow_name(p.shadow_config.filter_mode));
    printf("    frame_alloc     : %zu MB (x%u flights)\n",
           p.memory_config.frame_allocator_size / MB,
           p.memory_config.flight_count);
    printf("    mesh_pool       : %zu MB\n",
           p.memory_config.gpu_config.mesh_pool_size / MB);
    printf("    ssbo_pool       : %zu MB\n",
           p.memory_config.gpu_config.ssbo_pool_size / MB);
    printf("    passes (%zu)    :", p.render_passes.size());
    for (const auto& rp : p.render_passes) printf(" %s", rp.pass_name.c_str());
    printf("\n    post_process    :");
    if (p.post_process_stack.empty()) printf(" (none)");
    for (const auto& pp : p.post_process_stack) {
        printf(" %s%s", pp.name.c_str(), pp.enabled ? "" : "(disabled)");
    }
    printf("\n");
}

// ---- Key/value config loader (INI-lite: one "key=value" per line) ---------

std::unordered_map<std::string, std::string> load_kv_config(const std::string& path) {
    std::unordered_map<std::string, std::string> kv;
    std::ifstream f(path);
    if (!f.is_open()) {
        fprintf(stderr, "warning: could not open config '%s'\n", path.c_str());
        return kv;
    }
    std::string line;
    while (std::getline(f, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = line.substr(0, eq);
        auto val = line.substr(eq + 1);
        auto trim = [](std::string& s) {
            auto b = s.find_first_not_of(" \t\r\n");
            auto e = s.find_last_not_of(" \t\r\n");
            if (b == std::string::npos) { s.clear(); return; }
            s = s.substr(b, e - b + 1);
        };
        trim(key); trim(val);
        if (!key.empty()) kv[key] = val;
    }
    return kv;
}

// ---- Frame simulation -----------------------------------------------------

void run_frames(PictorRenderer& renderer, uint32_t frame_count) {
    const Camera cam = make_camera();
    constexpr float DT = 1.0f / 60.0f;

    auto t0 = std::chrono::high_resolution_clock::now();
    for (uint32_t f = 0; f < frame_count; ++f) {
        renderer.begin_frame(DT);
        renderer.render(cam);
        renderer.end_frame();
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const auto& s = renderer.get_frame_stats();
    printf("    ran %u frames in %.1f ms (%.2f ms/frame)\n", frame_count, ms, ms / frame_count);
    printf("    last-frame: fps=%.1f update=%.2fms cull=%.2fms batch=%.2fms\n",
           s.fps, s.data_update_ms, s.culling_ms, s.batch_build_ms);
}

} // namespace

// ---- Entry point ----------------------------------------------------------

int main(int argc, char* argv[]) {
    printf("=== Pictor Mobile Rendering Pipeline Demo ===\n\n");

    std::string start_profile = "MobileLow";
    std::string config_path;
    uint32_t    frames  = 120;
    int32_t     objects = -1; // -1 = pick per-profile default
    bool        cycle   = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--profile=", 0) == 0)      start_profile = arg.substr(10);
        else if (arg.rfind("--frames=", 0) == 0)  frames  = static_cast<uint32_t>(std::atoi(arg.c_str() + 9));
        else if (arg.rfind("--objects=", 0) == 0) objects = std::atoi(arg.c_str() + 10);
        else if (arg.rfind("--config=", 0) == 0)  config_path = arg.substr(9);
        else if (arg == "--switch")               cycle = true;
        else if (arg == "--help" || arg == "-h") {
            printf("usage: pictor_mobile_demo [--profile=<name>] [--frames=N] [--objects=N] [--switch] [--config=<path>]\n");
            return 0;
        }
    }

    // ---- Build the profile set ---------------------------------------------

    PipelineProfileDef low  = PipelineProfileManager::create_mobile_low_profile();
    PipelineProfileDef high = PipelineProfileManager::create_mobile_high_profile();

    // Demonstrate external construction: start from MobileHigh and tweak a
    // handful of knobs via the fluent builder.
    PipelineProfileDef custom = PipelineProfileBuilder::from_preset(high)
        .with_msaa(0)
        .with_max_lights(24)
        .with_shadow(1, 512, ShadowFilterMode::PCF)
        .with_memory_budget_mb(2, 96, 24)
        .clear_post_process()
        .add_post_process("Tonemapping")
        .add_post_process("FXAA")
        .take();
    custom.profile_name = "MobileCustom";

    // Optional data-driven profile loaded from a flat kv file.
    PipelineProfileDef from_file;
    bool has_from_file = false;
    if (!config_path.empty()) {
        auto kv = load_kv_config(config_path);
        if (!kv.empty()) {
            from_file = PipelineProfileBuilder::from_key_value(low, kv);
            has_from_file = true;
        }
    }

    printf("-- Registered mobile profiles ------------------------------------------\n");
    print_profile(low);
    print_profile(high);
    print_profile(custom);
    if (has_from_file) {
        printf("\n  (loaded from %s)\n", config_path.c_str());
        print_profile(from_file);
    }
    printf("\n");

    // ---- Wire up renderer --------------------------------------------------

    RendererConfig rc;
    rc.initial_profile = low.profile_name; // begin with the smallest memory footprint
    rc.memory_config   = low.memory_config;
    rc.update_config   = low.update_config;
    rc.screen_width    = 1280;
    rc.screen_height   = 720;
    rc.profiler_enabled = true;
    rc.overlay_mode     = OverlayMode::MINIMAL;

    PictorRenderer renderer;
    renderer.initialize(rc);

    // Register custom profiles with the manager — this is the external hand-off.
    renderer.register_custom_profile(low);
    renderer.register_custom_profile(high);
    renderer.register_custom_profile(custom);
    if (has_from_file) renderer.register_custom_profile(from_file);

    MobileUpdateCallback cb;
    renderer.set_update_callback(&cb);

    // ---- Scene -------------------------------------------------------------

    uint32_t obj_count = (objects >= 0)
        ? static_cast<uint32_t>(objects)
        : (start_profile == "MobileLow" ? 2000u : 20000u);
    printf("Registering %u scene objects...\n", obj_count);
    register_scene(renderer, obj_count);
    printf("\n");

    // ---- Run ---------------------------------------------------------------

    std::vector<std::string> run_list;
    if (cycle) {
        run_list = {"MobileLow", "MobileHigh", "MobileCustom"};
        if (has_from_file) run_list.push_back(from_file.profile_name);
    } else {
        run_list = {start_profile};
    }

    for (const auto& name : run_list) {
        if (!renderer.set_profile(name)) {
            fprintf(stderr, "warning: profile '%s' not registered — skipping\n", name.c_str());
            continue;
        }
        printf("-- Active profile: %s ------------------------------------------\n", name.c_str());
        run_frames(renderer, frames);
        printf("\n");
    }

    // ---- Mobile lifecycle demonstration -------------------------------------
    //
    // Exercises pause → resume, thermal throttling (with auto-downgrade),
    // surface loss / regain and low-memory notifications. Nothing here
    // requires a GPU — the renderer's internal state machine drives the
    // observer and profile switch.

    class LifecycleLog : public pictor::IMobileLifecycleObserver {
    public:
        void on_lifecycle_change(pictor::LifecycleState prev,
                                 pictor::LifecycleState next) override {
            printf("    [lifecycle] %s → %s\n", label(prev), label(next));
        }
        void on_thermal_change(pictor::ThermalState prev,
                               pictor::ThermalState next) override {
            printf("    [thermal]   %s → %s\n", t_label(prev), t_label(next));
        }
        void on_memory_pressure(pictor::MemoryPressure level) override {
            const char* l = "normal";
            if (level == pictor::MemoryPressure::MODERATE) l = "moderate";
            if (level == pictor::MemoryPressure::CRITICAL) l = "critical";
            printf("    [memory]    pressure=%s\n", l);
        }
    private:
        static const char* label(pictor::LifecycleState s) {
            switch (s) {
                case pictor::LifecycleState::ACTIVE:       return "ACTIVE";
                case pictor::LifecycleState::PAUSED:       return "PAUSED";
                case pictor::LifecycleState::SUSPENDED:    return "SUSPENDED";
                case pictor::LifecycleState::SURFACE_LOST: return "SURFACE_LOST";
            }
            return "?";
        }
        static const char* t_label(pictor::ThermalState t) {
            switch (t) {
                case pictor::ThermalState::NOMINAL:   return "NOMINAL";
                case pictor::ThermalState::FAIR:      return "FAIR";
                case pictor::ThermalState::SERIOUS:   return "SERIOUS";
                case pictor::ThermalState::CRITICAL:  return "CRITICAL";
                case pictor::ThermalState::EMERGENCY: return "EMERGENCY";
            }
            return "?";
        }
    };

    printf("-- Mobile lifecycle simulation ----------------------------------\n");

    LifecycleLog log;
    renderer.set_lifecycle_observer(&log);

    // Enable auto-downgrade: thermal SERIOUS swaps to MobileLow.
    pictor::MobileAutoDowngradePolicy policy;
    policy.enabled           = true;
    policy.low_profile_name  = "MobileLow";
    policy.high_profile_name = "MobileHigh";
    policy.downgrade_at      = pictor::ThermalState::SERIOUS;
    policy.restore_below     = pictor::ThermalState::FAIR;
    renderer.set_mobile_downgrade_policy(policy);

    renderer.set_profile("MobileHigh");
    printf("    active profile before events: %s\n", renderer.current_profile_name().c_str());

    // App goes into the switcher: 10 frames of attempted work are no-ops.
    renderer.on_pause();
    run_frames(renderer, 10);
    renderer.on_resume();

    // Thermal warning: auto-downgrade fires.
    renderer.on_thermal_state(pictor::ThermalState::SERIOUS);
    printf("    after SERIOUS:  active profile = %s\n", renderer.current_profile_name().c_str());

    // Cooled down: restore.
    renderer.on_thermal_state(pictor::ThermalState::NOMINAL);
    printf("    after NOMINAL:  active profile = %s\n", renderer.current_profile_name().c_str());

    // Android onSurfaceDestroyed / iOS backgrounded.
    renderer.on_surface_lost();
    run_frames(renderer, 5);  // suppressed
    renderer.on_surface_regained();

    // Memory warning.
    renderer.on_low_memory(pictor::MemoryPressure::MODERATE);
    renderer.on_low_memory(pictor::MemoryPressure::CRITICAL);

    renderer.set_lifecycle_observer(nullptr);
    printf("\n");

    renderer.shutdown();
    printf("Mobile demo complete.\n");
    return 0;
}
