/// Pictor Rive Renderer Demo
///
/// Plays one of the bundled sample `.riv` files through
/// pictor::RiveRenderer (which wraps Rive's own GPU renderer). The demo
/// ships 5 samples under `rive/sample1.riv` .. `rive/sample5.riv` next
/// to the build output (same sibling level as `textures/` / `shaders/`
/// / `fonts/`), and lets you cycle between them at runtime to verify
/// that the Rive runtime integration handles file import, artboard
/// selection, state machine advancement, and Vulkan frame plumbing
/// end-to-end inside Pictor.
///
/// Usage:
///   pictor_rive_demo              — start with sample1.riv
///   pictor_rive_demo 3            — start with sample3.riv
///   pictor_rive_demo path.riv     — start with any custom .riv file
///
/// Controls:
///   1..5            — jump to sample{N}.riv from the rive/ folder
///   Space           — toggle between state machine 0 and animation 0
///   Left / Right    — cycle artboards within the current file
///   Esc             — quit

#include "pictor/surface/vulkan_context.h"
#include "pictor/surface/glfw_surface_provider.h"
#include "pictor/vector/rive_renderer.h"

#include <GLFW/glfw3.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// Diagnostic logs in Debug builds only. Release = silent.
#ifdef NDEBUG
#  define DEMO_DBG(...) ((void)0)
#else
#  define DEMO_DBG(...) std::printf(__VA_ARGS__)
#endif

using namespace pictor;

namespace {

/// Returns the first existing path among several candidates, or empty.
/// The staged asset directory sits next to the demo exe (copied by
/// CMake as `pictor_rive_assets`, mirroring `pictor_texture2d_assets`
/// / `pictor_fonts`), but depending on how the binary is invoked the
/// cwd may be the exe directory, the build root, or the repo root.
/// We probe each, in that order.
std::string resolve_rive_dir() {
    namespace fs = std::filesystem;
    const std::vector<fs::path> candidates = {
        fs::current_path() / "rive",             // cwd/rive
        fs::current_path() / ".." / "rive",      // Release/.. /rive (= build/rive)
        fs::current_path() / ".." / ".." / "rive",
    };
    for (const auto& p : candidates) {
        std::error_code ec;
        if (fs::exists(p, ec) && fs::is_directory(p, ec)) {
            return fs::absolute(p, ec).string();
        }
    }
    return "";
}

std::string resolve_sample(int index, const std::string& rive_dir) {
    if (rive_dir.empty()) return "";
    char name[64];
    std::snprintf(name, sizeof(name), "sample%d.riv", index);
    std::filesystem::path p = std::filesystem::path(rive_dir) / name;
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) {
        return p.string();
    }
    return "";
}

struct Keys {
    bool toggle_scene   = false;
    int  cycle_artboard = 0; // -1, 0, +1
    int  switch_sample  = 0; // 0 = none, 1..5 = requested index
};
static Keys g_keys;

void key_callback(GLFWwindow* w, int key, int /*scan*/, int action, int /*mods*/) {
    if (action != GLFW_PRESS) return;
    switch (key) {
    case GLFW_KEY_ESCAPE: glfwSetWindowShouldClose(w, GLFW_TRUE); break;
    case GLFW_KEY_SPACE:  g_keys.toggle_scene = true;             break;
    case GLFW_KEY_LEFT:   g_keys.cycle_artboard = -1;             break;
    case GLFW_KEY_RIGHT:  g_keys.cycle_artboard = +1;             break;
    case GLFW_KEY_1:      g_keys.switch_sample = 1;                break;
    case GLFW_KEY_2:      g_keys.switch_sample = 2;                break;
    case GLFW_KEY_3:      g_keys.switch_sample = 3;                break;
    case GLFW_KEY_4:      g_keys.switch_sample = 4;                break;
    case GLFW_KEY_5:      g_keys.switch_sample = 5;                break;
    default: break;
    }
}

/// Load a .riv and (re)configure state machine / animation selection.
/// Returns true on success and prints a one-line descriptor.
bool load_and_select(RiveRenderer& rive, const std::string& riv_path,
                     int requested_sm, bool& using_sm_out) {
    if (!rive.load_riv_file(riv_path)) {
        fprintf(stderr, "[rive-demo] failed to load %s\n", riv_path.c_str());
        return false;
    }
    // Prefer the requested state machine; fall back to animation 0.
    using_sm_out = rive.set_state_machine(requested_sm);
    if (!using_sm_out) {
        rive.set_animation(0);
    }
    DEMO_DBG("[rive-demo] loaded: %s (%s)\n", riv_path.c_str(),
           using_sm_out ? "state-machine 0" : "animation 0");
    return true;
}

} // namespace

int main(int argc, char** argv) {
    // Unbuffered stdout so diagnostic prints reach the terminal / redirect
    // even when the process is killed with SIGTERM from a test harness.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    DEMO_DBG("[rive-demo] ==== boot ====\n");
    DEMO_DBG("[rive-demo] argv[0]=%s  cwd=%s\n",
           argv[0], std::filesystem::current_path().string().c_str());

    std::string rive_dir = resolve_rive_dir();
    if (rive_dir.empty()) {
        fprintf(stderr,
                "[rive-demo] warning: rive/ asset directory not found.\n"
                "  tried: ./rive, ../rive, ../../rive (relative to cwd)\n"
                "  Samples sample1..sample5.riv are expected in the build dir next\n"
                "  to textures/ / fonts/ / shaders/ (copied by CMake target\n"
                "  `pictor_rive_assets`).\n");
    } else {
        DEMO_DBG("[rive-demo] sample dir: %s\n", rive_dir.c_str());
        for (int i = 1; i <= 5; ++i) {
            std::string p = resolve_sample(i, rive_dir);
            DEMO_DBG("[rive-demo]   sample%d -> %s\n", i,
                   p.empty() ? "(not found)" : p.c_str());
        }
    }

    // Argument resolution:
    //   (none)       → sample1.riv (if dir found)
    //   <digit 1-5>  → sample<N>.riv
    //   <path>       → literal path
    std::string riv_path;
    int current_sample_idx = 0;  // 0 = custom / none, 1..5 = sample index
    if (argc >= 2) {
        std::string a = argv[1];
        if (a.size() == 1 && a[0] >= '1' && a[0] <= '5') {
            current_sample_idx = a[0] - '0';
            riv_path = resolve_sample(current_sample_idx, rive_dir);
        } else {
            riv_path = a;
        }
    } else if (!rive_dir.empty()) {
        current_sample_idx = 1;
        riv_path = resolve_sample(1, rive_dir);
    }
    if (riv_path.empty()) {
        fprintf(stderr,
                "usage: %s [<1..5> | <path/to/file.riv>]\n\n"
                "Built-in samples are installed under `<build-dir>/rive/sample{1..5}.riv`.\n",
                argv[0]);
        return 1;
    }
    int requested_sm = (argc >= 3) ? std::atoi(argv[2]) : 0;

    printf("=== Pictor Rive Renderer Demo ===\n");
    printf("file: %s\n", riv_path.c_str());
    printf("state machine: %d\n\n", requested_sm);

    // ─── 1. GLFW + Vulkan ───────────────────────────────────
    DEMO_DBG("[rive-demo] creating GLFW window...\n");
    GlfwSurfaceProvider surface;
    GlfwWindowConfig win_cfg;
    win_cfg.width  = 1024;
    win_cfg.height = 1024;
    win_cfg.title  = "Pictor — Rive Renderer Demo";
    win_cfg.vsync  = true;
    if (!surface.create(win_cfg)) {
        fprintf(stderr, "[rive-demo] GLFW window creation failed\n");
        return 1;
    }
    DEMO_DBG("[rive-demo] GLFW window ok\n");
    glfwSetKeyCallback(surface.glfw_window(), key_callback);

    DEMO_DBG("[rive-demo] initializing VulkanContext...\n");
    VulkanContext vk;
    VulkanContextConfig vk_cfg;
    vk_cfg.app_name   = "Pictor Rive Demo";
    vk_cfg.validation = true;
    if (!vk.initialize(&surface, vk_cfg)) {
        fprintf(stderr, "[rive-demo] Vulkan init failed\n");
        surface.destroy();
        return 1;
    }
    DEMO_DBG("[rive-demo] Vulkan ok: extent=%ux%u format=%d\n",
           vk.swapchain_extent().width, vk.swapchain_extent().height,
           (int)vk.swapchain_format());

    // ─── 2. Rive renderer ──────────────────────────────────
    DEMO_DBG("[rive-demo] initializing RiveRenderer...\n");
    RiveRenderer rive;
    RiveRenderer::Options opts;
    // Atomic mode is the portable path. Flip to false once Pictor's device
    // creation is verified to enable Rive's preferred rasterizer-ordering
    // extensions (VK_EXT_rasterization_order_attachment_access etc.).
    opts.force_atomic_mode = true;
    opts.clear_color       = 0xff202020; // dark grey background

    if (!rive.initialize(vk, opts)) {
        fprintf(stderr,
                "[rive-demo] RiveRenderer init failed. Common causes:\n"
                "  * Pictor was built without PICTOR_ENABLE_RIVE=ON\n"
                "  * rive-runtime was not built in release mode\n"
                "  * Required Vulkan device extensions are missing\n");
        vk.shutdown();
        surface.destroy();
        return 1;
    }
    DEMO_DBG("[rive-demo] RiveRenderer initialized\n");

    bool using_sm = false;
    if (!load_and_select(rive, riv_path, requested_sm, using_sm)) {
        fprintf(stderr, "[rive-demo] load_riv_file failed — exiting\n");
        rive.shutdown();
        vk.shutdown();
        surface.destroy();
        return 1;
    }

    printf("Controls: 1..5=sample, Space=toggle scene, Left/Right=cycle artboard, Esc=quit\n\n");

    // ─── 3. Main loop ──────────────────────────────────────
    auto     t_prev         = std::chrono::high_resolution_clock::now();
    // Rive frame accounting:
    //   currentFrameNumber = the frame about to be recorded
    //   safeFrameNumber    = largest frame whose GPU work is COMPLETE
    //                        (Pictor's acquire_next_image waits on the in-flight
    //                        fence, so at iteration start the previous frame is
    //                        always complete.)
    // Start at 1 so safe=0 refers to a synthetic pre-frame that never allocated
    // anything. This avoids Rive from destroying "frame 0" resources while we
    // are still recording frame 0.
    uint64_t frame_number   = 1;
    uint64_t frame_count    = 0;
    int      artboard_index = 0;

    while (!surface.should_close()) {
        surface.poll_events();

        auto t_now = std::chrono::high_resolution_clock::now();
        float dt   = std::chrono::duration<float>(t_now - t_prev).count();
        t_prev     = t_now;
        if (dt > 0.1f) dt = 0.1f; // clamp huge hitches

        // Handle sample switch
        if (g_keys.switch_sample != 0) {
            int want = g_keys.switch_sample;
            g_keys.switch_sample = 0;
            if (want != current_sample_idx) {
                std::string p = resolve_sample(want, rive_dir);
                if (p.empty()) {
                    fprintf(stderr, "[rive-demo] sample%d.riv not found in %s\n",
                            want, rive_dir.c_str());
                } else if (load_and_select(rive, p, requested_sm, using_sm)) {
                    current_sample_idx = want;
                    artboard_index     = 0;
                }
            }
        }
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
        if (image_idx == UINT32_MAX) {
            static int miss = 0;
            if (miss++ < 3) DEMO_DBG("[rive-demo] acquire_next_image returned UINT32_MAX (skip)\n");
            continue;
        }
        if (frame_count < 3) DEMO_DBG("[rive-demo] frame=%llu img_idx=%u\n",
                                     (unsigned long long)frame_count, image_idx);

        VkCommandBuffer cmd = vk.command_buffers()[image_idx];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &bi);

        VkImage image = nullptr;
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
        const uint32_t current_frame = static_cast<uint32_t>(frame_number);
        const uint32_t safe_frame    = static_cast<uint32_t>(frame_number - 1);
        rive.render(cmd,
                    image,
                    view,
                    vk.swapchain_extent(),
                    vk.swapchain_format(),
                    current_frame,
                    safe_frame,
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

        ++frame_number;
        ++frame_count;

        if (frame_count % 120 == 0) {
            DEMO_DBG("[frame %llu] %.1f fps\n",
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
