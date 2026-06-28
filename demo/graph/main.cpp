/// Pictor — Iter Relation Graph Demo (Step 2 + dock)
///
/// A native dock shell (split / tabs / leaf, draggable splitters, persisted
/// layout) hosting the Tier B graph widget in one leaf. The graph view culls to
/// the visible subset each frame (O(visible)) and hit-tests hover through a
/// spatial grid — the scale path web graph renderers (React Flow, used by Iter)
/// lack. Panel/tab contents are solid color blocks for now (text = a later step).
///
/// Controls:
///   Drag (in graph)     : pan
///   Scroll (in graph)   : zoom toward cursor
///   Drag splitter       : resize panes
///   Click tab header    : switch tab
///   R                   : reset graph view (re-fit)
///   S                   : save dock layout
///   Esc                 : quit (also auto-saves layout)
///
/// Args:
///   --nodes  N  : node count        (default 10000)
///   --frames N  : auto-exit after N frames, 0 = run until closed (default 0)

#include "pictor/surface/vulkan_context.h"
#include "pictor/surface/glfw_surface_provider.h"
#include "pictor/profiler/bitmap_text_renderer.h"
#include "camera2d.h"
#include "dock_layout.h"
#include "graph_generator.h"
#include "graph_instances.h"
#include "graph_store.h"
#include "graph_view.h"
#include "synthetic_data_channel.h"
#include "ui_rect_renderer.h"

#include <GLFW/glfw3.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace pictor;
using namespace pictor::graph;

namespace {

// Panel ids and their placeholder chrome colors (graph panel draws itself).
constexpr int kPanelGraph     = 0;
constexpr int kPanelInspector = 1;
constexpr int kPanelConsole   = 2;
const char*   kLayoutFile     = "dock_layout.txt";

struct AppState {
    GraphView*  graph = nullptr;
    DockLayout* dock  = nullptr;

    bool   graph_panning = false;
    bool   dock_dragging = false;
    double last_x = 0.0, last_y = 0.0;
    double cursor_x = 0.0, cursor_y = 0.0;

    // Click vs drag discrimination for node select / expand.
    double     press_x = 0.0, press_y = 0.0;
    NodeHandle press_node = INVALID_NODE;

    bool   save_request  = false;
    bool   reset_request = false;
};

constexpr float kClickSlopPx = 4.0f;  // movement under this = click, not pan

AppState g_state;

uint32_t panel_color(int panel_id) {
    switch (panel_id) {
    case kPanelInspector: return 0x1B2230FF; // dark slate
    case kPanelConsole:   return 0x15181EFF; // near-black
    default:              return 0x101218FF; // neutral dark
    }
}

const char* panel_name(int panel_id) {
    switch (panel_id) {
    case kPanelGraph:     return "Graph";
    case kPanelInspector: return "Inspector";
    case kPanelConsole:   return "Console";
    default:              return "Panel";
    }
}

void unpack(uint32_t rgba, float out[4]) {
    out[0] = ((rgba >> 24) & 0xFF) / 255.0f;
    out[1] = ((rgba >> 16) & 0xFF) / 255.0f;
    out[2] = ((rgba >> 8)  & 0xFF) / 255.0f;
    out[3] = ( rgba        & 0xFF) / 255.0f;
}

VkRect2D to_vk(const DockRect& r) {
    VkRect2D v;
    v.offset.x = static_cast<int32_t>(r.x < 0 ? 0 : r.x);
    v.offset.y = static_cast<int32_t>(r.y < 0 ? 0 : r.y);
    v.extent.width  = static_cast<uint32_t>(r.w < 0 ? 0 : r.w);
    v.extent.height = static_cast<uint32_t>(r.h < 0 ? 0 : r.h);
    return v;
}

void push_rect(std::vector<UiRect>& out, const DockRect& r, uint32_t rgba) {
    UiRect u;
    u.rect[0] = r.x; u.rect[1] = r.y; u.rect[2] = r.w; u.rect[3] = r.h;
    unpack(rgba, u.color);
    out.push_back(u);
}

// ─── GLFW callbacks ──────────────────────────────────────────

void mouse_button_callback(GLFWwindow* win, int button, int action, int) {
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;
    double x = g_state.cursor_x, y = g_state.cursor_y;
    glfwGetCursorPos(win, &x, &y);

    if (action == GLFW_PRESS) {
        const float fx = static_cast<float>(x), fy = static_cast<float>(y);
        if (g_state.dock->begin_drag(fx, fy)) {
            g_state.dock_dragging = true;
        } else if (g_state.dock->on_click(fx, fy)) {
            // tab switched
        } else if (g_state.graph->contains(fx, fy)) {
            g_state.graph_panning = true;
            g_state.last_x = x;
            g_state.last_y = y;
            g_state.press_x = x;
            g_state.press_y = y;
            g_state.press_node = g_state.graph->pick(fx, fy);
        }
    } else { // RELEASE
        // A press+release on a node without dragging = select + expand it. Pure
        // pan drags (cursor moved past the slop) never select, so the view is
        // not jumped by an accidental click.
        const double mdx = x - g_state.press_x;
        const double mdy = y - g_state.press_y;
        const bool is_click = (mdx * mdx + mdy * mdy) <= kClickSlopPx * kClickSlopPx;
        if (g_state.graph_panning && is_click && g_state.press_node != INVALID_NODE) {
            g_state.graph->select(g_state.press_node);
            g_state.graph->expand_node(g_state.press_node);
        }
        g_state.press_node    = INVALID_NODE;
        g_state.graph_panning = false;
        g_state.dock_dragging = false;
        g_state.dock->end_drag();
    }
}

void cursor_pos_callback(GLFWwindow*, double x, double y) {
    g_state.cursor_x = x;
    g_state.cursor_y = y;
    const float fx = static_cast<float>(x), fy = static_cast<float>(y);

    if (g_state.dock_dragging) {
        g_state.dock->drag_to(fx, fy);
    } else if (g_state.graph_panning) {
        g_state.graph->pan(static_cast<float>(x - g_state.last_x),
                           static_cast<float>(y - g_state.last_y));
        g_state.last_x = x;
        g_state.last_y = y;
    }

    if (g_state.graph->contains(fx, fy))
        g_state.graph->update_hover(fx, fy);
    else
        g_state.graph->clear_hover();
}

void scroll_callback(GLFWwindow* win, double, double yoffset) {
    double x = 0, y = 0;
    glfwGetCursorPos(win, &x, &y);
    const float fx = static_cast<float>(x), fy = static_cast<float>(y);
    if (!g_state.graph->contains(fx, fy)) return;
    const float factor = (yoffset > 0) ? 1.15f : (1.0f / 1.15f);
    g_state.graph->zoom_at(fx, fy, factor);
}

void key_callback(GLFWwindow* win, int key, int, int action, int) {
    if (action != GLFW_PRESS) return;
    if (key == GLFW_KEY_R)      g_state.reset_request = true;
    if (key == GLFW_KEY_S)      g_state.save_request  = true;
    if (key == GLFW_KEY_ESCAPE) glfwSetWindowShouldClose(win, GLFW_TRUE);
}

struct Args {
    uint32_t nodes   = 10000;
    uint64_t frames  = 0;
    bool     scatter = false;
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--nodes") == 0 && i + 1 < argc)
            a.nodes = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            a.frames = std::strtoull(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--scatter") == 0)
            a.scatter = true;
    }
    if (a.nodes == 0) a.nodes = 1;
    return a;
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    printf("=== Pictor Iter Relation Graph Demo (Step 5: snippet LOD + LRU) ===\n");
    printf("[init] Nodes: %u  frames: %llu  scatter: %s\n", args.nodes,
           static_cast<unsigned long long>(args.frames), args.scatter ? "yes" : "no");

    // Window
    GlfwSurfaceProvider surface_provider;
    GlfwWindowConfig win_cfg;
    win_cfg.width  = 1280;
    win_cfg.height = 720;
    win_cfg.title  = "Pictor — Iter Relation Graph (Step 5: snippet LOD + LRU)";
    win_cfg.vsync  = true;
    if (!surface_provider.create(win_cfg)) {
        fprintf(stderr, "[init] FATAL: window\n");
        return 1;
    }

    // Vulkan
    VulkanContext vk_ctx;
    VulkanContextConfig vk_cfg;
    vk_cfg.app_name   = "Pictor Iter Graph Demo";
    vk_cfg.validation = true;
    if (!vk_ctx.initialize(&surface_provider, vk_cfg)) {
        fprintf(stderr, "[init] FATAL: vulkan\n");
        surface_provider.destroy();
        return 1;
    }

    // Graph widget
    GraphView graph;
    if (!graph.initialize(vk_ctx, "shaders", generate_layered_graph(args.nodes, 1u, args.scatter))) {
        fprintf(stderr, "[init] FATAL: graph view\n");
        vk_ctx.shutdown();
        surface_provider.destroy();
        return 1;
    }
    printf("[init] Graph: %u nodes, %u edges\n", graph.total_nodes(), graph.total_edges());

    // Source of children for click-to-expand (Step 6). Synthetic today; swap for
    // ClangdDataChannel once Iter's IPC bridge is wired.
    SyntheticDataChannel channel(args.nodes, 1u);
    graph.set_data_channel(&channel);

    // Dock chrome renderer
    UiRectRenderer ui;
    if (!ui.initialize(vk_ctx, "shaders", 256)) {
        fprintf(stderr, "[init] FATAL: ui renderer\n");
        graph.shutdown();
        vk_ctx.shutdown();
        surface_provider.destroy();
        return 1;
    }

    // Text renderer (embedded 8x16 monospace bitmap font) — labels for nodes
    // (LABEL LOD) and dock chrome (tab / panel names).
    BitmapTextRenderer text;
    if (!text.initialize(vk_ctx, "shaders")) {
        fprintf(stderr, "[init] FATAL: text renderer\n");
        ui.shutdown();
        graph.shutdown();
        vk_ctx.shutdown();
        surface_provider.destroy();
        return 1;
    }

    // Dock layout — load persisted, else default
    DockLayout dock;
    if (dock.load(kLayoutFile))
        printf("[init] Loaded dock layout from %s\n", kLayoutFile);
    else
        dock.set_default(kPanelGraph, {kPanelInspector, kPanelConsole}, 0.72f);

    g_state.graph = &graph;
    g_state.dock  = &dock;

    GLFWwindow* win = surface_provider.glfw_window();
    glfwSetMouseButtonCallback(win, mouse_button_callback);
    glfwSetCursorPosCallback(win, cursor_pos_callback);
    glfwSetScrollCallback(win, scroll_callback);
    glfwSetKeyCallback(win, key_callback);

    printf("[loop] Drag=pan, Scroll=zoom, click node=select+expand, drag splitter=resize, click tab=switch, R=fit, S=save, Esc=quit\n");

    uint64_t frame_count = 0;
    auto fps_t0 = std::chrono::steady_clock::now();
    uint64_t fps_frames = 0;

    while (!surface_provider.should_close()) {
        surface_provider.poll_events();

        if (g_state.reset_request) { g_state.reset_request = false; graph.reset_view(); }
        if (g_state.save_request)  { g_state.save_request  = false; dock.save(kLayoutFile);
                                     printf("[dock] Saved layout to %s\n", kLayoutFile); }

        const VkExtent2D ext = vk_ctx.swapchain_extent();
        const float vw = static_cast<float>(ext.width);
        const float vh = static_cast<float>(ext.height);

        // Solve dock for the full window, then place the graph widget.
        dock.solve(DockRect{0, 0, vw, vh});
        for (const auto& p : dock.panels())
            if (p.panel_id == kPanelGraph)
                graph.set_bounds(to_vk(p.rect));

        // Build dock chrome rects (non-graph panel backgrounds + tabs + splitters).
        std::vector<UiRect> chrome;
        chrome.reserve(32);
        for (const auto& p : dock.panels())
            if (p.panel_id != kPanelGraph)
                push_rect(chrome, p.rect, panel_color(p.panel_id));
        for (const auto& t : dock.tabs())
            push_rect(chrome, t.rect, t.active ? 0x2C3850FF : 0x1A1F2AFF);
        for (const auto& s : dock.splitters())
            push_rect(chrome, s.handle, 0x39414FFF);

        float ui_proj[16];
        Camera2D::build_proj(ui_proj, vw, vh);

        const uint32_t image_idx = vk_ctx.acquire_next_image();
        if (image_idx == UINT32_MAX) continue;
        const uint32_t flight = vk_ctx.current_frame();

        VkCommandBuffer cmd = vk_ctx.command_buffers()[image_idx];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &begin_info);

        VkClearValue clear_color = {{{0.07f, 0.08f, 0.10f, 1.0f}}};
        VkRenderPassBeginInfo rp{};
        rp.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass      = vk_ctx.default_render_pass();
        rp.framebuffer     = vk_ctx.framebuffers()[image_idx];
        rp.renderArea      = {{0, 0}, ext};
        rp.clearValueCount = 1;
        rp.pClearValues    = &clear_color;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

        graph.render(cmd, flight);
        ui.draw(cmd, ext, ui_proj, flight, chrome.data(),
                static_cast<uint32_t>(chrome.size()));

        // Text pass: dock chrome labels (tab + panel names) then node labels.
        text.begin(cmd, ext);
        text.set_scale(1.0f);
        for (const auto& t : dock.tabs()) {
            const char* nm = panel_name(t.panel_id);
            const float tw = static_cast<float>(std::strlen(nm)) * 8.0f;
            const float tx = t.rect.x + (t.rect.w - tw) * 0.5f;
            const float ty = t.rect.y + (t.rect.h - 16.0f) * 0.5f;
            if (t.active) text.draw_text(tx, ty, nm, 0.92f, 0.96f, 1.0f, 1.0f);
            else          text.draw_text(tx, ty, nm, 0.55f, 0.60f, 0.68f, 1.0f);
        }
        for (const auto& p : dock.panels()) {
            if (p.panel_id == kPanelGraph) continue;
            text.draw_text(p.rect.x + 10.0f, p.rect.y + 8.0f, panel_name(p.panel_id),
                           0.70f, 0.76f, 0.86f, 1.0f);
        }
        graph.draw_labels(text);
        text.end();

        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphore wait_sem = vk_ctx.image_available_semaphore();
        VkSemaphore sig_sem  = vk_ctx.render_finished_semaphore();
        VkSubmitInfo submit{};
        submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount   = 1;
        submit.pWaitSemaphores      = &wait_sem;
        submit.pWaitDstStageMask    = &wait_stage;
        submit.commandBufferCount   = 1;
        submit.pCommandBuffers      = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores    = &sig_sem;
        vkQueueSubmit(vk_ctx.graphics_queue(), 1, &submit, vk_ctx.in_flight_fence());

        vk_ctx.present(image_idx);

        ++frame_count;
        ++fps_frames;
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - fps_t0).count();
        if (elapsed >= 1.0) {
            printf("[loop] FPS %.1f | visible %u/%u nodes, %u/%u edges | zoom %.3f | hover %d | snip$ %u\n",
                   fps_frames / elapsed,
                   graph.visible_nodes(), graph.total_nodes(),
                   graph.visible_edges(), graph.total_edges(),
                   graph.zoom(), static_cast<int>(graph.hovered()),
                   graph.snippet_cache_size());
            fps_t0 = now;
            fps_frames = 0;
        }

        if (args.frames != 0 && frame_count >= args.frames) {
            printf("[loop] Reached --frames, exiting\n");
            break;
        }
    }

    // Persist layout on exit.
    dock.save(kLayoutFile);

    vk_ctx.device_wait_idle();
    text.shutdown();
    ui.shutdown();
    graph.shutdown();
    vk_ctx.shutdown();
    surface_provider.destroy();
    printf("[cleanup] Done. %llu frames.\n",
           static_cast<unsigned long long>(frame_count));
    return 0;
}
