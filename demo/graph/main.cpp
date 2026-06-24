/// Pictor — Iter Relation Graph Demo (Step 1)
///
/// Renders a large synthetic node/edge graph with an instanced/DoD pipeline to
/// prove that ~10k boxes + edges pan/zoom smoothly on the desktop — the case
/// web graph renderers (React Flow et al., used by Iter) choke on. Real clangd
/// data, layout solving, labels and snippets come in later steps.
///
/// Controls:
///   Drag        : pan
///   Scroll      : zoom (towards cursor)
///   R           : reset view
///   Esc         : quit
///
/// Args:
///   --nodes  N  : node count        (default 10000)
///   --frames N  : auto-exit after N frames, 0 = run until closed (default 0)

#include "pictor/surface/vulkan_context.h"
#include "pictor/surface/glfw_surface_provider.h"
#include "graph_store.h"
#include "graph_generator.h"
#include "graph_renderer.h"
#include "camera2d.h"

#include <GLFW/glfw3.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace pictor;
using namespace pictor::graph;

// ─── Input state ─────────────────────────────────────────────

struct AppState {
    Camera2D camera;
    bool     dragging     = false;
    double   last_mouse_x = 0.0;
    double   last_mouse_y = 0.0;
    float    viewport_w   = 1280.0f;
    float    viewport_h   = 720.0f;
    bool     reset_view   = false;

    // Initial "fit whole graph" framing, captured at startup so R re-fits.
    float    fit_cx       = 0.0f;
    float    fit_cy       = 0.0f;
    float    fit_zoom     = 1.0f;
};

static AppState g_state;

static void mouse_button_callback(GLFWwindow* win, int button, int action, int) {
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        g_state.dragging = (action == GLFW_PRESS);
        if (g_state.dragging)
            glfwGetCursorPos(win, &g_state.last_mouse_x, &g_state.last_mouse_y);
    }
}

static void cursor_pos_callback(GLFWwindow*, double x, double y) {
    if (!g_state.dragging) return;
    const float dx = static_cast<float>(x - g_state.last_mouse_x);
    const float dy = static_cast<float>(y - g_state.last_mouse_y);
    g_state.camera.pan_pixels(dx, dy);
    g_state.last_mouse_x = x;
    g_state.last_mouse_y = y;
}

static void scroll_callback(GLFWwindow* win, double, double yoffset) {
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(win, &mx, &my);
    const float factor = (yoffset > 0) ? 1.15f : (1.0f / 1.15f);
    g_state.camera.zoom_at(static_cast<float>(mx), static_cast<float>(my),
                           factor, g_state.viewport_w, g_state.viewport_h);
}

static void key_callback(GLFWwindow* win, int key, int, int action, int) {
    if (action != GLFW_PRESS) return;
    if (key == GLFW_KEY_R)      g_state.reset_view = true;
    if (key == GLFW_KEY_ESCAPE) glfwSetWindowShouldClose(win, GLFW_TRUE);
}

// ─── Args ────────────────────────────────────────────────────

struct Args {
    uint32_t nodes  = 10000;
    uint64_t frames = 0; // 0 = run until window closed
};

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--nodes") == 0 && i + 1 < argc)
            a.nodes = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            a.frames = std::strtoull(argv[++i], nullptr, 10);
    }
    if (a.nodes == 0) a.nodes = 1;
    return a;
}

// ─── Main ────────────────────────────────────────────────────

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    printf("=== Pictor Iter Relation Graph Demo (Step 1) ===\n");
    printf("[init] Nodes requested: %u  (frames: %llu)\n",
           args.nodes, static_cast<unsigned long long>(args.frames));

    // ── Window ───────────────────────────────────────────────
    GlfwSurfaceProvider surface_provider;
    GlfwWindowConfig win_cfg;
    win_cfg.width  = 1280;
    win_cfg.height = 720;
    win_cfg.title  = "Pictor — Iter Relation Graph (Step 1)";
    win_cfg.vsync  = true;
    if (!surface_provider.create(win_cfg)) {
        fprintf(stderr, "[init] FATAL: Failed to create GLFW window\n");
        return 1;
    }

    // ── Vulkan ───────────────────────────────────────────────
    VulkanContext vk_ctx;
    VulkanContextConfig vk_cfg;
    vk_cfg.app_name   = "Pictor Iter Graph Demo";
    vk_cfg.validation = true;
    if (!vk_ctx.initialize(&surface_provider, vk_cfg)) {
        fprintf(stderr, "[init] FATAL: Failed to initialize Vulkan context\n");
        surface_provider.destroy();
        return 1;
    }
    printf("[init] Vulkan ready. Swapchain: %ux%u\n",
           vk_ctx.swapchain_extent().width, vk_ctx.swapchain_extent().height);

    // ── Graph data ───────────────────────────────────────────
    GraphStore store = generate_layered_graph(args.nodes);
    printf("[init] Generated graph: %zu nodes, %zu edges\n",
           store.node_count(), store.edge_count());

    // ── Renderer ─────────────────────────────────────────────
    GraphRenderer graph_renderer;
    if (!graph_renderer.initialize(vk_ctx, "shaders", store)) {
        fprintf(stderr, "[init] FATAL: Failed to initialize graph renderer\n");
        vk_ctx.shutdown();
        surface_provider.destroy();
        return 1;
    }

    // Frame the whole graph: center the camera, pick a zoom that fits.
    {
        const auto& n = store.nodes();
        float min_x = 0, max_x = 0, min_y = 0, max_y = 0;
        if (store.node_count() > 0) {
            min_x = max_x = n.cx[0];
            min_y = max_y = n.cy[0];
            for (size_t i = 1; i < store.node_count(); ++i) {
                min_x = (n.cx[i] < min_x) ? n.cx[i] : min_x;
                max_x = (n.cx[i] > max_x) ? n.cx[i] : max_x;
                min_y = (n.cy[i] < min_y) ? n.cy[i] : min_y;
                max_y = (n.cy[i] > max_y) ? n.cy[i] : max_y;
            }
        }
        const float span_x = (max_x - min_x) + 200.0f;
        const float span_y = (max_y - min_y) + 200.0f;
        const float zx = static_cast<float>(win_cfg.width)  / span_x;
        const float zy = static_cast<float>(win_cfg.height) / span_y;
        g_state.fit_cx   = (min_x + max_x) * 0.5f;
        g_state.fit_cy   = (min_y + max_y) * 0.5f;
        g_state.fit_zoom = (zx < zy) ? zx : zy;
        g_state.camera.reset(g_state.fit_cx, g_state.fit_cy, g_state.fit_zoom);
    }

    // ── Callbacks ────────────────────────────────────────────
    GLFWwindow* win = surface_provider.glfw_window();
    glfwSetMouseButtonCallback(win, mouse_button_callback);
    glfwSetCursorPosCallback(win, cursor_pos_callback);
    glfwSetScrollCallback(win, scroll_callback);
    glfwSetKeyCallback(win, key_callback);

    // ── Loop ─────────────────────────────────────────────────
    printf("[loop] Controls: Drag=pan, Scroll=zoom, R=reset, Esc=quit\n");
    uint64_t frame_count = 0;
    auto fps_t0 = std::chrono::steady_clock::now();
    uint64_t fps_frames = 0;

    while (!surface_provider.should_close()) {
        surface_provider.poll_events();

        if (g_state.reset_view) {
            // R re-fits the whole graph using the framing captured at startup.
            g_state.reset_view = false;
            g_state.camera.reset(g_state.fit_cx, g_state.fit_cy, g_state.fit_zoom);
        }

        const VkExtent2D ext = vk_ctx.swapchain_extent();
        g_state.viewport_w = static_cast<float>(ext.width);
        g_state.viewport_h = static_cast<float>(ext.height);

        GraphPushConstants pc{};
        Camera2D::build_proj(pc.proj, g_state.viewport_w, g_state.viewport_h);
        g_state.camera.build_view(pc.view, g_state.viewport_w, g_state.viewport_h);
        pc.params[0] = g_state.camera.zoom();

        const uint32_t image_idx = vk_ctx.acquire_next_image();
        if (image_idx == UINT32_MAX) continue; // swapchain recreated

        VkCommandBuffer cmd = vk_ctx.command_buffers()[image_idx];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &begin_info);

        VkClearValue clear_color = {{{0.09f, 0.10f, 0.13f, 1.0f}}};
        VkRenderPassBeginInfo rp_info{};
        rp_info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_info.renderPass      = vk_ctx.default_render_pass();
        rp_info.framebuffer     = vk_ctx.framebuffers()[image_idx];
        rp_info.renderArea      = {{0, 0}, ext};
        rp_info.clearValueCount = 1;
        rp_info.pClearValues    = &clear_color;
        vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

        graph_renderer.render(cmd, ext, pc);

        vkCmdEndRenderPass(cmd);
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

        ++frame_count;
        ++fps_frames;
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - fps_t0).count();
        if (elapsed >= 1.0) {
            printf("[loop] FPS: %.1f | nodes: %u edges: %u | zoom: %.3f\n",
                   fps_frames / elapsed, graph_renderer.node_count(),
                   graph_renderer.edge_count(), g_state.camera.zoom());
            fps_t0 = now;
            fps_frames = 0;
        }

        if (args.frames != 0 && frame_count >= args.frames) {
            printf("[loop] Reached --frames %llu, exiting\n",
                   static_cast<unsigned long long>(args.frames));
            break;
        }
    }

    // ── Cleanup (reverse order) ──────────────────────────────
    vk_ctx.device_wait_idle();
    graph_renderer.shutdown();
    vk_ctx.shutdown();
    surface_provider.destroy();
    printf("[cleanup] Done. %llu frames.\n",
           static_cast<unsigned long long>(frame_count));
    return 0;
}
