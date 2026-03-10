/// Pictor 1M Spheres Benchmark (§14)
///
/// Stress test: 1,000,000 independent spheres moving on 3D Lissajous curves.
/// All updates via GPU Compute Shader. CPU frame time target: < 0.5ms.

#include "pictor/pictor.h"
#include <cstdio>
#include <cmath>
#include <chrono>

using namespace pictor;

static constexpr uint32_t OBJECT_COUNT   = 1000000;
static constexpr float    GRID_SPACING   = 2.0f;
static constexpr uint32_t GRID_DIM       = 100;     // 100^3 = 1M
static constexpr uint32_t BENCHMARK_FRAMES = 600;    // 10 seconds at 60fps

/// CPU-side update callback for comparison testing (§14.7: CPU Update 1M variant)
class BenchmarkUpdateCallback : public IUpdateCallback {
public:
    float total_time = 0.0f;

    void update(float4x4* transforms, AABB* bounds,
                uint32_t start, uint32_t count, float delta_time) override {
        total_time += delta_time;

        for (uint32_t i = 0; i < count; ++i) {
            uint32_t global_idx = start + i;

            // Derive initial position from 3D grid index (§14.2)
            uint32_t ix = global_idx % GRID_DIM;
            uint32_t iy = (global_idx / GRID_DIM) % GRID_DIM;
            uint32_t iz = global_idx / (GRID_DIM * GRID_DIM);

            float base_x = static_cast<float>(ix) * GRID_SPACING - 100.0f;
            float base_y = static_cast<float>(iy) * GRID_SPACING - 100.0f;
            float base_z = static_cast<float>(iz) * GRID_SPACING - 100.0f;

            // Lissajous curve parameters from object index (§14.3)
            float hash = static_cast<float>(global_idx) * 0.001f;
            float freq_x = 1.0f + std::sin(hash * 1.1f) * 0.5f;
            float freq_y = 1.0f + std::sin(hash * 2.3f) * 0.5f;
            float freq_z = 1.0f + std::sin(hash * 3.7f) * 0.5f;
            float phase_x = hash * 0.7f;
            float phase_y = hash * 1.3f;
            float phase_z = hash * 2.1f;
            float amplitude = 1.0f;

            // 3D Lissajous motion (§14.3)
            float x = base_x + amplitude * std::sin(total_time * freq_x + phase_x);
            float y = base_y + amplitude * std::sin(total_time * freq_y + phase_y);
            float z = base_z + amplitude * std::sin(total_time * freq_z + phase_z);

            transforms[start + i] = float4x4::identity();
            transforms[start + i].set_translation(x, y, z);

            // Update AABB (sphere radius = 0.5)
            bounds[start + i].min = {x - 0.5f, y - 0.5f, z - 0.5f};
            bounds[start + i].max = {x + 0.5f, y + 0.5f, z + 0.5f};
        }
    }
};

int main(int argc, char* argv[]) {
    printf("=== Pictor 1M Spheres Benchmark (§14) ===\n");
    printf("Objects: %u\n", OBJECT_COUNT);
    printf("Grid: %ux%ux%u, spacing %.1f\n", GRID_DIM, GRID_DIM, GRID_DIM, GRID_SPACING);
    printf("Frames: %u\n\n", BENCHMARK_FRAMES);

    // Determine benchmark variant
    bool use_gpu_compute = true;
    if (argc > 1 && std::string(argv[1]) == "--cpu") {
        use_gpu_compute = false;
        printf("Mode: CPU Update (Level 1 + Level 2)\n");
    } else {
        printf("Mode: GPU Compute Update (Level 3)\n");
    }

    // Initialize renderer with Ultra profile (§14.4)
    RendererConfig config;
    config.initial_profile = "Ultra";
    config.memory_config.frame_allocator_size = 64 * 1024 * 1024; // 64MB
    config.memory_config.flight_count = 3;
    config.profiler_enabled = true;
    config.overlay_mode = OverlayMode::DETAILED;

    PictorRenderer renderer;
    renderer.initialize(config);

    // Set up CPU update callback
    BenchmarkUpdateCallback update_callback;
    renderer.set_update_callback(&update_callback);

    printf("Registering %u objects...\n", OBJECT_COUNT);
    auto reg_start = std::chrono::high_resolution_clock::now();

    // Register 1M spheres (§14.2)
    for (uint32_t i = 0; i < OBJECT_COUNT; ++i) {
        uint32_t ix = i % GRID_DIM;
        uint32_t iy = (i / GRID_DIM) % GRID_DIM;
        uint32_t iz = i / (GRID_DIM * GRID_DIM);

        float x = static_cast<float>(ix) * GRID_SPACING - 100.0f;
        float y = static_cast<float>(iy) * GRID_SPACING - 100.0f;
        float z = static_cast<float>(iz) * GRID_SPACING - 100.0f;

        ObjectDescriptor desc;
        desc.mesh = 0;       // shared low-poly sphere mesh
        desc.material = 0;   // single material
        desc.transform = float4x4::identity();
        desc.transform.set_translation(x, y, z);
        desc.bounds.min = {x - 0.5f, y - 0.5f, z - 0.5f};
        desc.bounds.max = {x + 0.5f, y + 0.5f, z + 0.5f};
        desc.shaderKey = 0;
        desc.materialKey = 0;

        if (use_gpu_compute) {
            desc.flags = ObjectFlags::GPU_DRIVEN | ObjectFlags::INSTANCED;
        } else {
            desc.flags = ObjectFlags::DYNAMIC | ObjectFlags::INSTANCED;
        }

        renderer.register_object(desc);
    }

    auto reg_end = std::chrono::high_resolution_clock::now();
    double reg_ms = std::chrono::duration<double, std::milli>(reg_end - reg_start).count();
    printf("Registration: %.1f ms\n\n", reg_ms);

    // Camera setup (§14.2: orbit camera looking at center)
    Camera camera;
    camera.position = {0.0f, 150.0f, 250.0f};
    // Simple frustum for benchmark
    for (int i = 0; i < 6; ++i) {
        camera.frustum.planes[i].normal = {0, 0, 0};
        camera.frustum.planes[i].distance = 10000.0f;
    }
    // Set up a basic frustum that includes most of the scene
    camera.frustum.planes[0] = {{1, 0, 0}, 200.0f};   // left
    camera.frustum.planes[1] = {{-1, 0, 0}, 200.0f};  // right
    camera.frustum.planes[2] = {{0, 1, 0}, 200.0f};   // bottom
    camera.frustum.planes[3] = {{0, -1, 0}, 200.0f};  // top
    camera.frustum.planes[4] = {{0, 0, 1}, 0.1f};     // near
    camera.frustum.planes[5] = {{0, 0, -1}, 1000.0f}; // far

    // Begin profiler recording
    renderer.begin_profiler_recording("benchmark_results");

    printf("Running benchmark...\n");
    printf("%-8s %-10s %-10s %-12s %-10s %-10s\n",
           "Frame", "FPS", "CPU(ms)", "Update(ms)", "Cull(ms)", "Batch(ms)");
    printf("----------------------------------------------------------------------\n");

    float delta_time = 1.0f / 60.0f; // Fixed timestep

    for (uint32_t frame = 0; frame < BENCHMARK_FRAMES; ++frame) {
        renderer.begin_frame(delta_time);
        renderer.render(camera);
        renderer.end_frame();

        // Print stats every 60 frames
        if (frame % 60 == 0) {
            const auto& stats = renderer.get_frame_stats();
            printf("%-8u %-10.1f %-10.2f %-12.2f %-10.2f %-10.2f\n",
                   frame,
                   stats.fps,
                   stats.frame_time_ms,
                   stats.data_update_ms,
                   stats.culling_ms,
                   stats.batch_build_ms);
        }
    }

    // End recording and export results
    renderer.end_profiler_recording();
    renderer.export_profiler_json("benchmark_results.json");
    renderer.export_profiler_csv("benchmark_results.csv");
    renderer.export_profiler_chrome_tracing("benchmark_results_trace.json");

    // Print final summary
    const auto& final_stats = renderer.get_frame_stats();
    printf("\n=== Final Statistics ===\n");
    printf("Average FPS:           %.1f\n", final_stats.fps);
    printf("Frame Time:            %.2f ms\n", final_stats.frame_time_ms);
    printf("GPU Frame Time:        %.2f ms\n", final_stats.gpu_frame_time_ms);
    printf("Data Update:           %.2f ms\n", final_stats.data_update_ms);
    printf("Culling:               %.2f ms\n", final_stats.culling_ms);
    printf("Sort + Batch:          %.2f ms\n", final_stats.sort_ms + final_stats.batch_build_ms);
    printf("Draw Calls:            %u\n", final_stats.draw_call_count);
    printf("Triangles:             %llu\n",
           static_cast<unsigned long long>(final_stats.triangle_count));
    printf("Visible Objects:       %u / %u (%.1f%%)\n",
           final_stats.visible_objects,
           OBJECT_COUNT,
           100.0f * final_stats.visible_objects / OBJECT_COUNT);
    printf("Frame Alloc Used:      %.2f MB / %.2f MB\n",
           final_stats.frame_alloc_used / (1024.0 * 1024.0),
           final_stats.frame_alloc_capacity / (1024.0 * 1024.0));

    printf("\nBenchmark complete. Results exported to:\n");
    printf("  benchmark_results.json\n");
    printf("  benchmark_results.csv\n");
    printf("  benchmark_results_trace.json (Chrome Tracing)\n");

    renderer.shutdown();
    return 0;
}
