/// FPS baseline test: drive the Pictor data pipeline headless with a
/// realistic Dynamic-Pool workload and assert the steady-state CPU
/// frame time stays under a configurable budget.
///
/// Threshold can be overridden via the env var
/// `PICTOR_FPS_THRESHOLD_MS` (default: 20.0 ms ≈ 50 fps) so that slow
/// CI hosts can loosen the budget without recompiling.

#include "pictor/pictor.h"
#include "test_common.h"

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstring>

using namespace pictor;
using namespace pictor_test;

namespace {

constexpr uint32_t OBJECT_COUNT  = 50000;
constexpr uint32_t WARMUP_FRAMES = 30;
constexpr uint32_t MEASURE_FRAMES = 200;

class NoopUpdate : public IUpdateCallback {
public:
    float t = 0.0f;
    void update(float4x4* transforms, AABB* bounds,
                uint32_t start, uint32_t count, float dt) override {
        t += dt;
        // Cheap deterministic motion so the SoA streams actually get touched.
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t idx = start + i;
            float x = static_cast<float>(idx % 100) - 50.0f;
            float y = std::sin(t + static_cast<float>(idx) * 0.001f);
            float z = static_cast<float>((idx / 100) % 100) - 50.0f;
            transforms[idx] = float4x4::identity();
            transforms[idx].set_translation(x, y, z);
            bounds[idx].min = {x - 0.5f, y - 0.5f, z - 0.5f};
            bounds[idx].max = {x + 0.5f, y + 0.5f, z + 0.5f};
        }
    }
};

double env_threshold_ms() {
    if (const char* s = std::getenv("PICTOR_FPS_THRESHOLD_MS")) {
        double v = std::atof(s);
        if (v > 0.0) return v;
    }
    return 20.0;
}

} // namespace

int main() {
    const double threshold_ms = env_threshold_ms();
    std::printf("       budget: %.2f ms / frame  (%u objects, %u frames)\n",
                threshold_ms, OBJECT_COUNT, MEASURE_FRAMES);

    RendererConfig cfg;
    cfg.initial_profile               = "Standard";
    cfg.profiler_enabled              = true;
    cfg.overlay_mode                  = OverlayMode::OFF;
    cfg.memory_config.frame_allocator_size = 16 * 1024 * 1024;
    cfg.memory_config.flight_count    = 2;
    cfg.screen_width                  = 1280;
    cfg.screen_height                 = 720;

    PictorRenderer renderer;
    renderer.initialize(cfg);

    NoopUpdate cb;
    renderer.set_update_callback(&cb);

    // ---- Register objects in a 100x5x100 grid (Dynamic Pool) ----
    for (uint32_t i = 0; i < OBJECT_COUNT; ++i) {
        ObjectDescriptor desc;
        desc.mesh        = 0;
        desc.material    = 0;
        desc.shaderKey   = 0;
        desc.materialKey = 0;
        desc.flags       = ObjectFlags::DYNAMIC | ObjectFlags::INSTANCED;
        float x = static_cast<float>(i % 100) - 50.0f;
        float z = static_cast<float>((i / 100) % 100) - 50.0f;
        desc.transform = float4x4::identity();
        desc.transform.set_translation(x, 0.0f, z);
        desc.bounds.min = {x - 0.5f, -0.5f, z - 0.5f};
        desc.bounds.max = {x + 0.5f,  0.5f, z + 0.5f};
        renderer.register_object(desc);
    }

    // ---- Camera with very wide frustum so almost everything is visible ----
    Camera camera;
    camera.position = {0.0f, 50.0f, 100.0f};
    camera.frustum.planes[0] = {{ 1, 0, 0}, 1000.0f};
    camera.frustum.planes[1] = {{-1, 0, 0}, 1000.0f};
    camera.frustum.planes[2] = {{ 0, 1, 0}, 1000.0f};
    camera.frustum.planes[3] = {{ 0,-1, 0}, 1000.0f};
    camera.frustum.planes[4] = {{ 0, 0, 1}, 1000.0f};
    camera.frustum.planes[5] = {{ 0, 0,-1}, 1000.0f};

    const float dt = 1.0f / 60.0f;

    // ---- Warmup ----
    for (uint32_t f = 0; f < WARMUP_FRAMES; ++f) {
        renderer.begin_frame(dt);
        renderer.render(camera);
        renderer.end_frame();
    }

    // ---- Measure ----
    auto wall_start = std::chrono::high_resolution_clock::now();
    double sum_frame_ms = 0.0;
    double max_frame_ms = 0.0;
    uint32_t total_visible = 0;
    for (uint32_t f = 0; f < MEASURE_FRAMES; ++f) {
        renderer.begin_frame(dt);
        renderer.render(camera);
        renderer.end_frame();
        const auto& s = renderer.get_frame_stats();
        sum_frame_ms += s.frame_time_ms;
        if (s.frame_time_ms > max_frame_ms) max_frame_ms = s.frame_time_ms;
        total_visible += s.visible_objects;
    }
    auto wall_end = std::chrono::high_resolution_clock::now();
    double wall_sec =
        std::chrono::duration<double>(wall_end - wall_start).count();

    const double avg_frame_ms = sum_frame_ms / MEASURE_FRAMES;
    const double avg_fps      = avg_frame_ms > 0 ? 1000.0 / avg_frame_ms : 0.0;

    std::printf("       wall:        %.3f s\n", wall_sec);
    std::printf("       avg frame:   %.3f ms  (%.1f fps)\n",
                avg_frame_ms, avg_fps);
    std::printf("       worst frame: %.3f ms\n", max_frame_ms);
    std::printf("       avg visible: %u / %u\n",
                total_visible / MEASURE_FRAMES, OBJECT_COUNT);

    PT_ASSERT(avg_frame_ms > 0.0, "profiler reported non-zero frame time");
    PT_ASSERT(avg_frame_ms < threshold_ms,
              "average frame time within budget");
    // Worst-case allowed to be ~3x the average budget — flags pathological
    // spikes without flaking on a single GC / OS preemption.
    PT_ASSERT(max_frame_ms < threshold_ms * 3.0,
              "no pathological frame spike");

    renderer.shutdown();
    return report("fps_baseline_test");
}
