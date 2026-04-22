/// Pictor Rive Renderer Demo
///
/// Plays an official .riv file through pictor::RiveRenderer (which wraps
/// Rive's own GPU renderer). Useful to verify that the Rive runtime
/// integration — including file import, artboard selection, state machine
/// advancement, and Vulkan frame plumbing — works end-to-end inside Pictor.
///
/// Usage:
///   pictor_rive_demo <path/to/file.riv> [state_machine_index]
///
/// Official sample .riv files live in rive-runtime/gold/ and
/// rive-runtime/renderer/path_fiddle/rivs/. Point the demo at any of them.
///
/// Controls:
///   Space      — toggle between state machine 0 and animation 0
///   Left/Right — cycle artboards
///   Esc        — quit

#include "pictor/surface/vulkan_context.h"
#include "pictor/surface/glfw_surface_provider.h"
#include "pictor/vector/rive_renderer.h"

#include <GLFW/glfw3.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace pictor;

namespace {

struct Keys {
    bool toggle_scene = false;
    int  cycle_artboard = 0; // -1, 0, +1
};
static Keys g_keys;

void key_callback(GLFWwindow* w, int key, int /*scan*/, int action, int /*mods*/) {
    if (action != GLFW_PRESS) return;
    switch (key) {
    case GLFW_KEY_ESCAPE: glfwSetWindowShouldClose(w, GLFW_TRUE); break;
    case GLFW_KEY_SPACE:  g_keys.toggle_scene = true;             break;
    case GLFW_KEY_LEFT:   g_keys.cycle_artboard = -1;             break;
    case GLFW_KEY_RIGHT:  g_keys.cycle_artboard = +1;             break;
    default: break;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <path/to/file.riv> [state_machine_index]\n\n"
                "Tip: official sample .riv files ship with rive-runtime under\n"
                "  rive-runtime/gold/ and rive-runtime/renderer/path_fiddle/rivs/\n",
                argv[0]);
        return 1;
    }
    const char* riv_path = argv[1];
    int requested_sm    = (argc >= 3) ? std::atoi(argv[2]) : 0;

    printf("=== Pictor Rive Renderer Demo ===\n");
    printf("file: %s\n", riv_path);
    printf("state machine: %d\n\n", requested_sm);

    // ─── 1. GLFW + Vulkan ───────────────────────────────────
    GlfwSurfaceProvider surface;
    GlfwWindowConfig win_cfg;
    win_cfg.width  = 1024;
    win_cfg.height = 1024;
    win_cfg.title  = "Pictor — Rive Renderer Demo";
    win_cfg.vsync  = true;
    if (!surface.create(win_cfg)) {
        fprintf(stderr, "GLFW window creation failed\n");
        return 1;
    }
    glfwSetKeyCallback(surface.glfw_window(), key_callback);

    VulkanContext vk;
    VulkanContextConfig vk_cfg;
    vk_cfg.app_name   = "Pictor Rive Demo";
    vk_cfg.validation = true;
    if (!vk.initialize(&surface, vk_cfg)) {
        fprintf(stderr, "Vulkan init failed\n");
        surface.destroy();
        return 1;
    }

    // ─── 2. Rive renderer ──────────────────────────────────
    RiveRenderer rive;
    RiveRenderer::Options opts;
    // Atomic mode is the portable path. Flip to false once Pictor's device
    // creation is verified to enable Rive's preferred rasterizer-ordering
    // extensions (VK_EXT_rasterization_order_attachment_access etc.).
    opts.force_atomic_mode = true;
    opts.clear_color       = 0xff202020; // dark grey background

    if (!rive.initialize(vk, opts)) {
        fprintf(stderr,
                "RiveRenderer init failed. Common causes:\n"
                "  * Pictor was built without PICTOR_ENABLE_RIVE=ON\n"
                "  * rive-runtime was not built in release mode\n"
                "  * Required Vulkan device extensions are missing\n");
        vk.shutdown();
        surface.destroy();
        return 1;
    }

    if (!rive.load_riv_file(riv_path)) {
        fprintf(stderr, "failed to load .riv\n");
        rive.shutdown();
        vk.shutdown();
        surface.destroy();
        return 1;
    }

    // Prefer the requested state machine; fall back to animation 0.
    bool using_sm = rive.set_state_machine(requested_sm);
    if (!using_sm) {
        using_sm = rive.set_animation(0);
        printf("(no state machine %d — falling back to animation 0)\n",
               requested_sm);
    }

    printf("Controls: Space=toggle scene, Left/Right=cycle artboard, Esc=quit\n\n");

    // ─── 3. Main loop ──────────────────────────────────────
    auto     t_prev         = std::chrono::high_resolution_clock::now();
    uint64_t frame_number   = 0;
    uint64_t safe_frame     = 0;
    uint64_t frame_count    = 0;
    int      artboard_index = 0;

    while (!surface.should_close()) {
        surface.poll_events();

        auto t_now = std::chrono::high_resolution_clock::now();
        float dt   = std::chrono::duration<float>(t_now - t_prev).count();
        t_prev     = t_now;
        if (dt > 0.1f) dt = 0.1f; // clamp huge hitches

        // Handle input since last frame
        if (g_keys.toggle_scene) {
            g_keys.toggle_scene = false;
            using_sm = !using_sm;
            if (using_sm) rive.set_state_machine(0);
            else          rive.set_animation(0);
            printf("scene: %s\n", using_sm ? "state machine 0" : "animation 0");
        }
        if (g_keys.cycle_artboard != 0) {
            artboard_index += g_keys.cycle_artboard;
            if (artboard_index < 0) artboard_index = 0;
            g_keys.cycle_artboard = 0;
            if (rive.set_artboard(artboard_index)) {
                if (using_sm) rive.set_state_machine(0);
                else          rive.set_animation(0);
                printf("artboard: %d\n", artboard_index);
            } else {
                --artboard_index; // revert
            }
        }

        rive.advance(dt);

        uint32_t image_idx = vk.acquire_next_image();
        if (image_idx == UINT32_MAX) continue;

        VkCommandBuffer cmd = vk.command_buffers()[image_idx];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &bi);

        VkImage     image = nullptr; // VulkanContext only exposes views; we
                                     // look up the image via swapchain below.
        // The current Pictor VulkanContext doesn't expose swapchain_images()
        // directly — use vkGetSwapchainImagesKHR on the fly.
        {
            uint32_t count = 0;
            vkGetSwapchainImagesKHR(vk.device(), vk.swapchain(), &count, nullptr);
            std::vector<VkImage> images(count);
            vkGetSwapchainImagesKHR(vk.device(), vk.swapchain(), &count, images.data());
            image = images[image_idx];
        }
        VkImageView view = vk.swapchain_image_views()[image_idx];

        // Rive renders and finishes with a layout that depends on the
        // rendering path; the wrapper tracks that internally and emits the
        // correct PRESENT_SRC_KHR transition on our behalf.
        rive.render(cmd,
                    image,
                    view,
                    vk.swapchain_extent(),
                    vk.swapchain_format(),
                    static_cast<uint32_t>(frame_number),
                    static_cast<uint32_t>(safe_frame),
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphore wait_sem = vk.image_available_semaphore();
        VkSemaphore sig_sem  = vk.render_finished_semaphore();

        VkSubmitInfo si{};
        si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount   = 1;
        si.pWaitSemaphores      = &wait_sem;
        si.pWaitDstStageMask    = &wait_stage;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &sig_sem;
        vkQueueSubmit(vk.graphics_queue(), 1, &si, vk.in_flight_fence());

        vk.present(image_idx);

        // Rive uses currentFrameNumber as a monotonically increasing id and
        // safeFrameNumber as "last frame whose resources have been retired by
        // the GPU". With a single-fence-per-frame model we wait on the fence
        // at the top of the next iteration's acquire; that means by the time
        // we advance frame_number, (frame_number - 1) is safe.
        if (frame_number > 0) safe_frame = frame_number - 1;
        ++frame_number;
        ++frame_count;

        if (frame_count % 120 == 0) {
            printf("[frame %llu] %.1f fps\n",
                   static_cast<unsigned long long>(frame_count),
                   1.0f / dt);
        }
    }

    // ─── 4. Cleanup ─────────────────────────────────────────
    vk.device_wait_idle();
    rive.shutdown();
    vk.shutdown();
    surface.destroy();
    printf("\nDemo finished. %llu frames.\n",
           static_cast<unsigned long long>(frame_count));
    return 0;
}
