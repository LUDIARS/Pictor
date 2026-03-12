/// Pictor Vulkan Window Demo
///
/// Opens a GLFW window, initializes a Vulkan swapchain via
/// the ISurfaceProvider abstraction, and renders a clear-color
/// animation to prove the pipeline is running.

#include "pictor/pictor.h"
#include "pictor/surface/vulkan_context.h"
#include "pictor/surface/glfw_surface_provider.h"

#include <cstdio>
#include <cmath>
#include <chrono>

using namespace pictor;

// ---------------------------------------------------------------------------
// Animated clear-color: slowly cycles through hues so you can instantly
// see that the Vulkan pipeline is alive.
// ---------------------------------------------------------------------------
static void hue_to_rgb(float h, float& r, float& g, float& b) {
    float s = 0.7f, v = 0.9f;
    int   i = static_cast<int>(h * 6.0f);
    float f = h * 6.0f - static_cast<float>(i);
    float p = v * (1.0f - s);
    float q = v * (1.0f - f * s);
    float t = v * (1.0f - (1.0f - f) * s);
    switch (i % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        case 5: r = v; g = p; b = q; break;
        default: r = g = b = 0; break;
    }
}

int main() {
    printf("=== Pictor Vulkan Window Demo ===\n\n");

    // ---- 1. GLFW Window ----
    GlfwSurfaceProvider surface_provider;
    GlfwWindowConfig win_cfg;
    win_cfg.width  = 1280;
    win_cfg.height = 720;
    win_cfg.title  = "Pictor — Vulkan Window Demo";
    win_cfg.vsync  = true;

    if (!surface_provider.create(win_cfg)) {
        fprintf(stderr, "Failed to create GLFW window\n");
        return 1;
    }

    // ---- 2. Vulkan Context ----
    VulkanContext vk_ctx;
    VulkanContextConfig vk_cfg;
    vk_cfg.app_name   = "Pictor Demo";
    vk_cfg.validation = true; // enable validation in demo

    if (!vk_ctx.initialize(&surface_provider, vk_cfg)) {
        fprintf(stderr, "Failed to initialize Vulkan context\n");
        surface_provider.destroy();
        return 1;
    }

    printf("Vulkan context initialized successfully.\n");
    printf("Swapchain: %ux%u\n\n",
           vk_ctx.swapchain_extent().width,
           vk_ctx.swapchain_extent().height);

    // ---- 3. Pictor Renderer (pipeline only — no actual draw yet) ----
    RendererConfig pictor_cfg;
    pictor_cfg.initial_profile = "Standard";
    pictor_cfg.screen_width    = vk_ctx.swapchain_extent().width;
    pictor_cfg.screen_height   = vk_ctx.swapchain_extent().height;
    pictor_cfg.profiler_enabled = true;
    pictor_cfg.overlay_mode    = OverlayMode::MINIMAL;

    PictorRenderer renderer;
    renderer.initialize(pictor_cfg);

    // ---- 4. Register a few test objects so profiler has something to report ----
    constexpr uint32_t TEST_OBJECTS = 1000;
    for (uint32_t i = 0; i < TEST_OBJECTS; ++i) {
        float x = static_cast<float>(i % 10) * 2.0f - 10.0f;
        float y = static_cast<float>((i / 10) % 10) * 2.0f - 10.0f;
        float z = static_cast<float>(i / 100) * 2.0f - 10.0f;

        ObjectDescriptor desc;
        desc.mesh      = 0;
        desc.material  = 0;
        desc.transform = float4x4::identity();
        desc.transform.set_translation(x, y, z);
        desc.bounds.min = {x - 0.5f, y - 0.5f, z - 0.5f};
        desc.bounds.max = {x + 0.5f, y + 0.5f, z + 0.5f};
        desc.flags = ObjectFlags::DYNAMIC | ObjectFlags::INSTANCED;
        renderer.register_object(desc);
    }

    Camera camera;
    camera.position = {0.0f, 20.0f, 40.0f};
    for (int i = 0; i < 6; ++i) {
        camera.frustum.planes[i].normal   = {0, 0, 0};
        camera.frustum.planes[i].distance = 10000.0f;
    }
    camera.frustum.planes[0] = {{1, 0, 0}, 100.0f};
    camera.frustum.planes[1] = {{-1, 0, 0}, 100.0f};
    camera.frustum.planes[2] = {{0, 1, 0}, 100.0f};
    camera.frustum.planes[3] = {{0, -1, 0}, 100.0f};
    camera.frustum.planes[4] = {{0, 0, 1}, 0.1f};
    camera.frustum.planes[5] = {{0, 0, -1}, 500.0f};

    // ---- 5. Main loop ----
    auto start = std::chrono::high_resolution_clock::now();
    uint64_t frame_count = 0;

    printf("Entering main loop. Close the window to exit.\n");

    while (!surface_provider.should_close()) {
        surface_provider.poll_events();

        // Elapsed time for hue animation
        auto now = std::chrono::high_resolution_clock::now();
        float elapsed = std::chrono::duration<float>(now - start).count();
        float hue = std::fmod(elapsed * 0.1f, 1.0f); // one full cycle every 10s

        float r, g, b;
        hue_to_rgb(hue, r, g, b);

        // Acquire swapchain image
        uint32_t image_idx = vk_ctx.acquire_next_image();
        if (image_idx == UINT32_MAX) continue; // swapchain was recreated

        // Record command buffer: clear to animated colour
#ifdef PICTOR_HAS_VULKAN
        auto cmd = vk_ctx.command_buffers()[image_idx];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &begin_info);

        VkClearValue clear_color = {{{r, g, b, 1.0f}}};

        VkRenderPassBeginInfo rp_info{};
        rp_info.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_info.renderPass  = vk_ctx.default_render_pass();
        rp_info.framebuffer = vk_ctx.framebuffers()[image_idx];
        rp_info.renderArea  = {{0, 0}, vk_ctx.swapchain_extent()};
        rp_info.clearValueCount = 1;
        rp_info.pClearValues    = &clear_color;

        vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);
        // (Future: Pictor would record draw commands here)
        vkCmdEndRenderPass(cmd);

        vkEndCommandBuffer(cmd);

        // Submit
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphore wait_sem  = vk_ctx.image_available_semaphore();
        VkSemaphore sig_sem   = vk_ctx.render_finished_semaphore();

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
#endif

        // Present
        vk_ctx.present(image_idx);

        // Run Pictor pipeline (data-only, no GPU draw yet)
        float dt = 1.0f / 60.0f;
        renderer.begin_frame(dt);
        renderer.render(camera);
        renderer.end_frame();

        ++frame_count;

        // Print stats every 120 frames
        if (frame_count % 120 == 0) {
            const auto& stats = renderer.get_frame_stats();
            printf("[Frame %llu] FPS: %.1f  Visible: %u/%u  Batches: %u\n",
                   static_cast<unsigned long long>(frame_count),
                   stats.fps,
                   stats.visible_objects,
                   TEST_OBJECTS,
                   stats.batch_count);
        }
    }

    // ---- 6. Cleanup ----
    vk_ctx.device_wait_idle();
    renderer.shutdown();
    vk_ctx.shutdown();
    surface_provider.destroy();

    printf("\nDemo finished. %llu frames rendered.\n",
           static_cast<unsigned long long>(frame_count));
    return 0;
}
