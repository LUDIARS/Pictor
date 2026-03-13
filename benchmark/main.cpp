/// Pictor 1M Spheres Benchmark (§14)
///
/// Stress test: 1,000,000 independent spheres moving on 3D Lissajous curves.
/// All updates via GPU Compute Shader. CPU frame time target: < 0.5ms.

#include "pictor/pictor.h"
#include <cstdio>
#include <cmath>
#include <chrono>
#include <vector>
#include <string>

#ifdef PICTOR_HAS_VULKAN
#include "pictor/surface/vulkan_context.h"
#include "pictor/surface/glfw_surface_provider.h"
#include "pictor/surface/simple_renderer.h"
#endif

using namespace pictor;

static constexpr uint32_t OBJECT_COUNT     = 1000000;
static constexpr float    GRID_SPACING     = 2.0f;
static constexpr uint32_t GRID_DIM         = 100;     // 100^3 = 1M
static constexpr uint32_t BENCHMARK_FRAMES = 600;     // 10 seconds at 60fps

// Simple math helpers for view/projection matrices
namespace {

void mat4_identity(float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void mat4_look_at(float* out, const float* eye, const float* center, const float* up) {
    float fx = center[0] - eye[0], fy = center[1] - eye[1], fz = center[2] - eye[2];
    float fl = std::sqrt(fx*fx + fy*fy + fz*fz);
    fx /= fl; fy /= fl; fz /= fl;

    float sx = fy*up[2] - fz*up[1], sy = fz*up[0] - fx*up[2], sz = fx*up[1] - fy*up[0];
    float sl = std::sqrt(sx*sx + sy*sy + sz*sz);
    sx /= sl; sy /= sl; sz /= sl;

    float ux = sy*fz - sz*fy, uy = sz*fx - sx*fz, uz = sx*fy - sy*fx;

    mat4_identity(out);
    out[0] = sx;  out[4] = sy;  out[8]  = sz;  out[12] = -(sx*eye[0]+sy*eye[1]+sz*eye[2]);
    out[1] = ux;  out[5] = uy;  out[9]  = uz;  out[13] = -(ux*eye[0]+uy*eye[1]+uz*eye[2]);
    out[2] = -fx; out[6] = -fy; out[10] = -fz; out[14] = (fx*eye[0]+fy*eye[1]+fz*eye[2]);
    out[3] = 0;   out[7] = 0;   out[11] = 0;   out[15] = 1.0f;
}

void mat4_perspective(float* out, float fovy_rad, float aspect, float near_z, float far_z) {
    memset(out, 0, 16 * sizeof(float));
    float f = 1.0f / std::tan(fovy_rad * 0.5f);
    out[0]  = f / aspect;
    out[5]  = -f; // Vulkan Y-flip
    out[10] = far_z / (near_z - far_z);
    out[11] = -1.0f;
    out[14] = (near_z * far_z) / (near_z - far_z);
}

} // anonymous namespace

/// CPU-side update callback that also stores instance positions for rendering
class BenchmarkUpdateCallback : public IUpdateCallback {
public:
    float total_time = 0.0f;
    std::vector<float> instance_data; // vec4 per instance: x, y, z, scale

    void update(float4x4* transforms, AABB* bounds,
                uint32_t start, uint32_t count, float delta_time) override {
        total_time += delta_time;

        for (uint32_t i = 0; i < count; ++i) {
            uint32_t global_idx = start + i;

            uint32_t ix = global_idx % GRID_DIM;
            uint32_t iy = (global_idx / GRID_DIM) % GRID_DIM;
            uint32_t iz = global_idx / (GRID_DIM * GRID_DIM);

            float base_x = static_cast<float>(ix) * GRID_SPACING - 100.0f;
            float base_y = static_cast<float>(iy) * GRID_SPACING - 100.0f;
            float base_z = static_cast<float>(iz) * GRID_SPACING - 100.0f;

            float hash = static_cast<float>(global_idx) * 0.001f;
            float freq_x = 1.0f + std::sin(hash * 1.1f) * 0.5f;
            float freq_y = 1.0f + std::sin(hash * 2.3f) * 0.5f;
            float freq_z = 1.0f + std::sin(hash * 3.7f) * 0.5f;
            float phase_x = hash * 0.7f;
            float phase_y = hash * 1.3f;
            float phase_z = hash * 2.1f;
            float amplitude = 1.0f;

            float x = base_x + amplitude * std::sin(total_time * freq_x + phase_x);
            float y = base_y + amplitude * std::sin(total_time * freq_y + phase_y);
            float z = base_z + amplitude * std::sin(total_time * freq_z + phase_z);

            transforms[start + i] = float4x4::identity();
            transforms[start + i].set_translation(x, y, z);

            bounds[start + i].min = {x - 0.5f, y - 0.5f, z - 0.5f};
            bounds[start + i].max = {x + 0.5f, y + 0.5f, z + 0.5f};

            // Store for GPU rendering
            uint32_t idx = (start + i) * 4;
            if (idx + 3 < static_cast<uint32_t>(instance_data.size())) {
                instance_data[idx + 0] = x;
                instance_data[idx + 1] = y;
                instance_data[idx + 2] = z;
                instance_data[idx + 3] = 0.5f; // radius
            }
        }
    }
};

int main(int argc, char* argv[]) {
    printf("=== Pictor 1M Spheres Benchmark (§14) ===\n");
    printf("Objects: %u\n", OBJECT_COUNT);
    printf("Grid: %ux%ux%u, spacing %.1f\n", GRID_DIM, GRID_DIM, GRID_DIM, GRID_SPACING);
    printf("Frames: %u\n\n", BENCHMARK_FRAMES);

    bool use_gpu_compute = true;
    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--cpu") use_gpu_compute = false;
        if (arg == "--headless") headless = true;
    }

    printf("Mode: %s\n", use_gpu_compute ? "GPU Compute Update (Level 3)" : "CPU Update (Level 1 + Level 2)");

#ifdef PICTOR_HAS_VULKAN
    GlfwSurfaceProvider surface_provider;
    VulkanContext vk_ctx;
    SimpleRenderer simple_renderer;

    if (!headless) {
        GlfwWindowConfig win_cfg;
        win_cfg.width  = 1280;
        win_cfg.height = 720;
        win_cfg.title  = "Pictor - 1M Spheres Benchmark";
        win_cfg.vsync  = false;

        if (!surface_provider.create(win_cfg)) {
            fprintf(stderr, "Failed to create GLFW window, falling back to headless\n");
            headless = true;
        }
    }

    if (!headless) {
        VulkanContextConfig vk_cfg;
        vk_cfg.app_name   = "Pictor Benchmark";
        vk_cfg.validation = false;

        if (!vk_ctx.initialize(&surface_provider, vk_cfg)) {
            fprintf(stderr, "Failed to initialize Vulkan, falling back to headless\n");
            headless = true;
        } else {
            printf("GPU: initialized (%ux%u)\n",
                   vk_ctx.swapchain_extent().width, vk_ctx.swapchain_extent().height);

            // Initialize SimpleRenderer with shader directory
            std::string shader_dir = "shaders";
            // Try relative to executable first
            if (!simple_renderer.initialize(vk_ctx, shader_dir.c_str())) {
                // Try build directory path
                shader_dir = std::string(argv[0]);
                auto pos = shader_dir.find_last_of("/\\");
                shader_dir = (pos != std::string::npos) ? shader_dir.substr(0, pos) + "/../shaders" : "shaders";
                if (!simple_renderer.initialize(vk_ctx, shader_dir.c_str())) {
                    fprintf(stderr, "Failed to init SimpleRenderer, falling back to headless\n");
                    headless = true;
                }
            }
        }
    }

    printf("Render: %s\n\n", headless ? "Headless (data pipeline only)" : "Vulkan Window");
#else
    headless = true;
    printf("Render: Headless (Vulkan not available)\n\n");
#endif

    // Initialize Pictor renderer
    RendererConfig config;
    config.initial_profile = "Ultra";
    config.memory_config.frame_allocator_size = 64 * 1024 * 1024;
    config.memory_config.flight_count = 3;
    config.profiler_enabled = true;
    config.overlay_mode = OverlayMode::DETAILED;

#ifdef PICTOR_HAS_VULKAN
    if (!headless) {
        config.screen_width  = vk_ctx.swapchain_extent().width;
        config.screen_height = vk_ctx.swapchain_extent().height;
    }
#endif

    PictorRenderer renderer;
    renderer.initialize(config);

    BenchmarkUpdateCallback update_callback;
    update_callback.instance_data.resize(OBJECT_COUNT * 4);
    renderer.set_update_callback(&update_callback);

    printf("Registering %u objects...\n", OBJECT_COUNT);
    auto reg_start = std::chrono::high_resolution_clock::now();

    for (uint32_t i = 0; i < OBJECT_COUNT; ++i) {
        uint32_t ix = i % GRID_DIM;
        uint32_t iy = (i / GRID_DIM) % GRID_DIM;
        uint32_t iz = i / (GRID_DIM * GRID_DIM);

        float x = static_cast<float>(ix) * GRID_SPACING - 100.0f;
        float y = static_cast<float>(iy) * GRID_SPACING - 100.0f;
        float z = static_cast<float>(iz) * GRID_SPACING - 100.0f;

        ObjectDescriptor desc;
        desc.mesh = 0;
        desc.material = 0;
        desc.transform = float4x4::identity();
        desc.transform.set_translation(x, y, z);
        desc.bounds.min = {x - 0.5f, y - 0.5f, z - 0.5f};
        desc.bounds.max = {x + 0.5f, y + 0.5f, z + 0.5f};
        desc.shaderKey = 0;
        desc.materialKey = 0;
        desc.flags = use_gpu_compute
            ? (ObjectFlags::GPU_DRIVEN | ObjectFlags::INSTANCED)
            : (ObjectFlags::DYNAMIC | ObjectFlags::INSTANCED);

        renderer.register_object(desc);

        // Initialize instance data
        update_callback.instance_data[i * 4 + 0] = x;
        update_callback.instance_data[i * 4 + 1] = y;
        update_callback.instance_data[i * 4 + 2] = z;
        update_callback.instance_data[i * 4 + 3] = 0.5f;
    }

    auto reg_end = std::chrono::high_resolution_clock::now();
    printf("Registration: %.1f ms\n\n",
           std::chrono::duration<double, std::milli>(reg_end - reg_start).count());

    // Camera
    Camera camera;
    camera.position = {0.0f, 150.0f, 250.0f};
    for (int i = 0; i < 6; ++i) {
        camera.frustum.planes[i].normal = {0, 0, 0};
        camera.frustum.planes[i].distance = 10000.0f;
    }
    camera.frustum.planes[0] = {{1, 0, 0}, 200.0f};
    camera.frustum.planes[1] = {{-1, 0, 0}, 200.0f};
    camera.frustum.planes[2] = {{0, 1, 0}, 200.0f};
    camera.frustum.planes[3] = {{0, -1, 0}, 200.0f};
    camera.frustum.planes[4] = {{0, 0, 1}, 0.1f};
    camera.frustum.planes[5] = {{0, 0, -1}, 1000.0f};

    // View/Projection matrices for rendering
    float view_mat[16], proj_mat[16];
    float eye[3] = {0.0f, 150.0f, 250.0f};
    float center[3] = {0.0f, 0.0f, 0.0f};
    float up[3] = {0.0f, 1.0f, 0.0f};
    mat4_look_at(view_mat, eye, center, up);

    renderer.begin_profiler_recording("benchmark_results");

    printf("Running benchmark...\n");
    printf("%-8s %-10s %-10s %-12s %-10s %-10s\n",
           "Frame", "FPS", "CPU(ms)", "Update(ms)", "Cull(ms)", "Batch(ms)");
    printf("----------------------------------------------------------------------\n");

    float delta_time = 1.0f / 60.0f;
    auto bench_start = std::chrono::high_resolution_clock::now();

    for (uint32_t frame = 0; frame < BENCHMARK_FRAMES; ++frame) {
        // Run Pictor data pipeline
        renderer.begin_frame(delta_time);
        renderer.render(camera);
        renderer.end_frame();

#ifdef PICTOR_HAS_VULKAN
        if (!headless) {
            surface_provider.poll_events();
            if (surface_provider.should_close()) break;

            // Compute perspective each frame (in case of resize)
            float aspect = static_cast<float>(vk_ctx.swapchain_extent().width)
                         / static_cast<float>(vk_ctx.swapchain_extent().height);
            mat4_perspective(proj_mat, 0.7854f, aspect, 0.1f, 2000.0f); // 45 deg FOV

            // Orbit camera
            float angle = static_cast<float>(frame) * 0.005f;
            eye[0] = 250.0f * std::sin(angle);
            eye[2] = 250.0f * std::cos(angle);
            mat4_look_at(view_mat, eye, center, up);

            // Upload instance positions
            simple_renderer.update_instances(update_callback.instance_data.data(), OBJECT_COUNT);

            // Acquire + record + submit
            uint32_t image_idx = vk_ctx.acquire_next_image();
            if (image_idx == UINT32_MAX) continue;

            auto cmd = vk_ctx.command_buffers()[image_idx];
            vkResetCommandBuffer(cmd, 0);

            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(cmd, &begin_info);

            simple_renderer.render(cmd, vk_ctx.default_render_pass(),
                                   vk_ctx.framebuffers()[image_idx],
                                   vk_ctx.swapchain_extent(),
                                   view_mat, proj_mat);

            vkEndCommandBuffer(cmd);

            VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkSemaphore wait_sem = vk_ctx.image_available_semaphore();
            VkSemaphore sig_sem  = vk_ctx.render_finished_semaphore();

            VkSubmitInfo submit_info{};
            submit_info.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit_info.waitSemaphoreCount   = 1;
            submit_info.pWaitSemaphores      = &wait_sem;
            submit_info.pWaitDstStageMask    = &wait_stage;
            submit_info.commandBufferCount   = 1;
            submit_info.pCommandBuffers      = &cmd;
            submit_info.signalSemaphoreCount = 1;
            submit_info.pSignalSemaphores    = &sig_sem;

            vkQueueSubmit(vk_ctx.graphics_queue(), 1, &submit_info, vk_ctx.in_flight_fence());
            vk_ctx.present(image_idx);
        }
#endif

        if (frame % 60 == 0) {
            const auto& stats = renderer.get_frame_stats();
            printf("%-8u %-10.1f %-10.2f %-12.2f %-10.2f %-10.2f\n",
                   frame, stats.fps, stats.frame_time_ms,
                   stats.data_update_ms, stats.culling_ms, stats.batch_build_ms);
        }
    }

    auto bench_end = std::chrono::high_resolution_clock::now();
    double bench_sec = std::chrono::duration<double>(bench_end - bench_start).count();

    renderer.end_profiler_recording();
    renderer.export_profiler_json("benchmark_results.json");
    renderer.export_profiler_csv("benchmark_results.csv");
    renderer.export_profiler_chrome_tracing("benchmark_results_trace.json");

    const auto& final_stats = renderer.get_frame_stats();
    printf("\n=== Final Statistics ===\n");
    printf("Wall Clock Time:       %.2f s\n", bench_sec);
    printf("Average FPS:           %.1f\n", final_stats.fps);
    printf("Frame Time:            %.2f ms\n", final_stats.frame_time_ms);
    printf("GPU Frame Time:        %.2f ms\n", final_stats.gpu_frame_time_ms);
    printf("Data Update:           %.2f ms\n", final_stats.data_update_ms);
    printf("Culling:               %.2f ms\n", final_stats.culling_ms);
    printf("Sort + Batch:          %.2f ms\n", final_stats.sort_ms + final_stats.batch_build_ms);
    printf("Draw Calls:            %u\n", final_stats.draw_call_count);
    printf("Visible Objects:       %u / %u (%.1f%%)\n",
           final_stats.visible_objects, OBJECT_COUNT,
           100.0f * final_stats.visible_objects / OBJECT_COUNT);
    printf("Render Mode:           %s\n", headless ? "Headless" : "Vulkan Window");

    printf("\nBenchmark complete. Results exported.\n");

#ifdef PICTOR_HAS_VULKAN
    if (!headless) {
        vk_ctx.device_wait_idle();
        simple_renderer.shutdown();
        vk_ctx.shutdown();
        surface_provider.destroy();
    }
#endif

    renderer.shutdown();
    return 0;
}
