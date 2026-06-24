#include "graph_view.h"

#ifdef PICTOR_HAS_VULKAN

#include "pictor/surface/vulkan_context.h"

#include <algorithm>
#include <string>
#include <utility>

namespace pictor::graph {

namespace {
inline void unpack_rgba(uint32_t rgba, float out[4]) {
    out[0] = static_cast<float>((rgba >> 24) & 0xFFu) / 255.0f;
    out[1] = static_cast<float>((rgba >> 16) & 0xFFu) / 255.0f;
    out[2] = static_cast<float>((rgba >> 8)  & 0xFFu) / 255.0f;
    out[3] = static_cast<float>( rgba        & 0xFFu) / 255.0f;
}
constexpr float kEdgeThicknessPx = 1.5f;

// Synthetic code snippet for a node (placeholder for clangd read_snippet).
// Deterministic from the symbol name + kind. Shaped as a few short lines so it
// reads like a definition inside the node card.
std::string make_synthetic_snippet(const std::string& name, uint8_t kind) {
    const char* ret = "void";
    switch (static_cast<NodeKind>(kind)) {
    case NodeKind::Function: ret = "int";    break;
    case NodeKind::Method:   ret = "auto";   break;
    case NodeKind::Variable: ret = "static"; break;
    case NodeKind::Type:     ret = "struct"; break;
    default:                 ret = "void";   break;
    }
    std::string s;
    if (static_cast<NodeKind>(kind) == NodeKind::Type) {
        s += "struct " + name + " {\n";
        s += "  int   id;\n";
        s += "  float value;\n";
        s += "};";
    } else if (static_cast<NodeKind>(kind) == NodeKind::Variable) {
        s += "static " + name + " =\n";
        s += "    compute();";
    } else {
        s += std::string(ret) + " " + name + "(int n) {\n";
        s += "  if (n <= 0) return 0;\n";
        s += "  return work(n) +\n";
        s += "         " + name + "(n - 1);\n";
        s += "}";
    }
    return s;
}
} // namespace

bool GraphView::initialize(VulkanContext& vk_ctx, const char* shader_dir, GraphStore store) {
    store_ = std::move(store);
    grid_.build(store_);

    const uint32_t nodes = static_cast<uint32_t>(store_.node_count());
    const uint32_t edges = static_cast<uint32_t>(store_.edge_count());
    if (!renderer_.initialize(vk_ctx, shader_dir, nodes, edges))
        return false;

    node_inst_.reserve(nodes);
    edge_inst_.reserve(edges);
    vis_stamp_.assign(nodes, 0);

    // Render starts at the generator positions and animates to the layout the
    // worker computes.
    anim_x_ = store_.nodes().cx;
    anim_y_ = store_.nodes().cy;

    fit_to_bounds();
    start_layout_worker();
    return true;
}

void GraphView::shutdown() {
    if (layout_thread_.joinable()) layout_thread_.join();
    renderer_.shutdown();
}

void GraphView::start_layout_worker() {
    // Copy topology so the worker is independent of any later store mutation.
    const uint32_t n = static_cast<uint32_t>(store_.node_count());
    std::vector<uint32_t> from = store_.edges().from;
    std::vector<uint32_t> to   = store_.edges().to;
    layout_thread_ = std::thread([this, n, from = std::move(from), to = std::move(to)]() {
        LayoutResult r = compute_layered_layout(n, from, to, {});
        layout_result_ = std::move(r);
        layout_ready_.store(true, std::memory_order_release);
    });
}

void GraphView::maybe_apply_layout() {
    if (layout_applied_) return;
    if (!layout_ready_.load(std::memory_order_acquire)) return;
    if (layout_thread_.joinable()) layout_thread_.join();
    layout_applied_ = true;
    if (!layout_result_.ok) return;

    // Target positions become authoritative; cull/hit-test use them immediately.
    store_.set_positions(layout_result_.x, layout_result_.y);
    grid_.build(store_);
    fit_to_bounds();           // re-frame to the new layout
    animating_ = true;         // render lerps anim_ -> store positions
}

void GraphView::advance_animation() {
    if (!animating_) return;
    const auto& n = store_.nodes();
    const float k = 0.18f;     // per-frame approach factor
    float max_delta = 0.0f;
    for (size_t i = 0; i < n.cx.size(); ++i) {
        const float dx = n.cx[i] - anim_x_[i];
        const float dy = n.cy[i] - anim_y_[i];
        anim_x_[i] += dx * k;
        anim_y_[i] += dy * k;
        const float ad = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        if (ad > max_delta) max_delta = ad;
    }
    if (max_delta < 0.5f) {    // settled — snap and stop the O(N) update
        anim_x_ = n.cx;
        anim_y_ = n.cy;
        animating_ = false;
    }
}

bool GraphView::contains(float win_x, float win_y) const {
    return win_x >= bounds_.offset.x &&
           win_x <  bounds_.offset.x + static_cast<int32_t>(bounds_.extent.width) &&
           win_y >= bounds_.offset.y &&
           win_y <  bounds_.offset.y + static_cast<int32_t>(bounds_.extent.height);
}

void GraphView::to_world(float win_x, float win_y, float& wx, float& wy) const {
    const float local_x = win_x - static_cast<float>(bounds_.offset.x);
    const float local_y = win_y - static_cast<float>(bounds_.offset.y);
    const float hw = bounds_.extent.width  * 0.5f;
    const float hh = bounds_.extent.height * 0.5f;
    const float z  = camera_.zoom();
    wx = camera_.center_x() + (local_x - hw) / z;
    wy = camera_.center_y() + (local_y - hh) / z;
}

void GraphView::pan(float dx, float dy) {
    camera_.pan_pixels(dx, dy);
}

void GraphView::zoom_at(float win_x, float win_y, float factor) {
    const float local_x = win_x - static_cast<float>(bounds_.offset.x);
    const float local_y = win_y - static_cast<float>(bounds_.offset.y);
    camera_.zoom_at(local_x, local_y, factor,
                    static_cast<float>(bounds_.extent.width),
                    static_cast<float>(bounds_.extent.height));
}

void GraphView::update_hover(float win_x, float win_y) {
    float wx, wy;
    to_world(win_x, win_y, wx, wy);
    hovered_ = grid_.pick(wx, wy);
}

void GraphView::reset_view() {
    fit_to_bounds();
}

void GraphView::fit_to_bounds() {
    const auto& n = store_.nodes();
    float min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    if (store_.node_count() > 0) {
        min_x = max_x = n.cx[0];
        min_y = max_y = n.cy[0];
        for (size_t i = 1; i < store_.node_count(); ++i) {
            if (n.cx[i] < min_x) min_x = n.cx[i];
            if (n.cx[i] > max_x) max_x = n.cx[i];
            if (n.cy[i] < min_y) min_y = n.cy[i];
            if (n.cy[i] > max_y) max_y = n.cy[i];
        }
    }
    const float span_x = (max_x - min_x) + 200.0f;
    const float span_y = (max_y - min_y) + 200.0f;
    const float vw = bounds_.extent.width  > 0 ? static_cast<float>(bounds_.extent.width)  : 1280.0f;
    const float vh = bounds_.extent.height > 0 ? static_cast<float>(bounds_.extent.height) : 720.0f;
    const float zx = vw / span_x;
    const float zy = vh / span_y;
    fit_cx_   = (min_x + max_x) * 0.5f;
    fit_cy_   = (min_y + max_y) * 0.5f;
    fit_zoom_ = (zx < zy) ? zx : zy;
    camera_.reset(fit_cx_, fit_cy_, fit_zoom_);
}

void GraphView::render(VkCommandBuffer cmd, uint32_t flight) {
    maybe_apply_layout();
    advance_animation();

    if (bounds_.extent.width == 0 || bounds_.extent.height == 0) return;

    const float vw = static_cast<float>(bounds_.extent.width);
    const float vh = static_cast<float>(bounds_.extent.height);
    const float z  = camera_.zoom();

    // View AABB in world units.
    const float min_x = camera_.center_x() - (vw * 0.5f) / z;
    const float max_x = camera_.center_x() + (vw * 0.5f) / z;
    const float min_y = camera_.center_y() - (vh * 0.5f) / z;
    const float max_y = camera_.center_y() + (vh * 0.5f) / z;

    grid_.query_visible(min_x, min_y, max_x, max_y, vis_nodes_);

    ++frame_;
    const auto& n = store_.nodes();
    node_inst_.clear();
    node_inst_.reserve(vis_nodes_.size());
    for (uint32_t i : vis_nodes_) {
        vis_stamp_[i] = frame_;
        NodeInstance d;
        // Render at the animated position (lerps to the layout target).
        d.rect[0] = anim_x_[i];
        d.rect[1] = anim_y_[i];
        d.rect[2] = n.w[i];
        d.rect[3] = n.h[i];
        if (i == hovered_) {
            // Highlight the hovered node with a bright accent.
            d.color[0] = 1.0f; d.color[1] = 0.85f; d.color[2] = 0.30f; d.color[3] = 1.0f;
        } else {
            unpack_rgba(n.rgba[i], d.color);
        }
        node_inst_.push_back(d);
    }

    // Edges visible if either endpoint is visible this frame.
    const auto& e = store_.edges();
    edge_inst_.clear();
    for (size_t k = 0; k < store_.edge_count(); ++k) {
        const uint32_t a = e.from[k];
        const uint32_t b = e.to[k];
        if (vis_stamp_[a] != frame_ && vis_stamp_[b] != frame_) continue;
        EdgeInstance d;
        d.ends[0] = anim_x_[a]; d.ends[1] = anim_y_[a];
        d.ends[2] = anim_x_[b]; d.ends[3] = anim_y_[b];
        d.color[0] = 0.45f; d.color[1] = 0.50f; d.color[2] = 0.58f;
        d.color[3] = kEdgeThicknessPx;
        edge_inst_.push_back(d);
    }

    last_vis_nodes_ = static_cast<uint32_t>(node_inst_.size());
    last_vis_edges_ = static_cast<uint32_t>(edge_inst_.size());

    GraphPushConstants pc{};
    Camera2D::build_proj(pc.proj, vw, vh);
    camera_.build_view(pc.view, vw, vh);
    pc.params[0] = z;

    renderer_.draw(cmd, bounds_, pc, flight,
                   node_inst_.data(), last_vis_nodes_,
                   edge_inst_.data(), last_vis_edges_);
}

void GraphView::draw_labels(BitmapTextRenderer& text) {
    if (bounds_.extent.width == 0 || bounds_.extent.height == 0) return;

    const auto& n = store_.nodes();
    const float z  = camera_.zoom();
    const float ox = static_cast<float>(bounds_.offset.x);
    const float oy = static_cast<float>(bounds_.offset.y);
    const float ew = static_cast<float>(bounds_.extent.width);
    const float eh = static_cast<float>(bounds_.extent.height);
    const float cx = camera_.center_x();
    const float cy = camera_.center_y();

    constexpr float kLabelMinPx   = 48.0f;   // >= this: show the symbol name
    constexpr float kSnippetMinPx = 200.0f;  // >= this: show the code snippet card
    int char_budget = 3600;                  // keep under BitmapTextRenderer's batch cap

    for (uint32_t i : vis_nodes_) {
        if (char_budget <= 0) break;
        const float w_px = n.w[i] * z;
        if (w_px < kLabelMinPx) continue;            // too small to read
        const std::string& name = n.label[i];
        if (name.empty()) continue;

        // Node center -> screen (animated position).
        const float scx = ox + (anim_x_[i] - cx) * z + ew * 0.5f;
        const float scy = oy + (anim_y_[i] - cy) * z + eh * 0.5f;
        if (scx < ox || scx > ox + ew || scy < oy || scy > oy + eh) continue;

        const float h_px = n.h[i] * z;

        if (w_px < kSnippetMinPx) {
            // ── LABEL LOD: centered symbol name ──
            const float scale = std::clamp(h_px * 0.42f / 16.0f, 0.6f, 2.2f);
            const float tw = static_cast<float>(name.size()) * 8.0f * scale;
            const float th = 16.0f * scale;
            text.set_scale(scale);
            text.draw_text(scx - tw * 0.5f, scy - th * 0.5f, name.c_str(),
                           0.92f, 0.95f, 1.0f, 1.0f);
            char_budget -= static_cast<int>(name.size());
        } else {
            // ── NEAR LOD: lazily-fetched snippet card (LRU-cached) ──
            const uint8_t kind = n.kind[i];
            std::string snip = snippets_.get(i, [&]() {
                return make_synthetic_snippet(name, kind);
            });
            const float scale = std::clamp(h_px / (7.0f * 16.0f * 1.25f), 0.7f, 1.6f);
            const float line_h = 16.0f * scale * 1.25f;
            const float left   = scx - w_px * 0.5f + 8.0f;
            float       ty     = scy - h_px * 0.5f + 6.0f;
            const float bottom = scy + h_px * 0.5f - 4.0f;

            size_t start = 0;
            while (start <= snip.size() && ty + line_h <= bottom && char_budget > 0) {
                size_t nl = snip.find('\n', start);
                const size_t end = (nl == std::string::npos) ? snip.size() : nl;
                std::string line = snip.substr(start, end - start);
                text.set_scale(scale);
                // First line (signature) brighter, body dimmer.
                if (start == 0)
                    text.draw_text(left, ty, line.c_str(), 0.95f, 0.97f, 1.0f, 1.0f);
                else
                    text.draw_text(left, ty, line.c_str(), 0.62f, 0.78f, 0.66f, 1.0f);
                char_budget -= static_cast<int>(line.size());
                ty += line_h;
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
        }
    }
}

} // namespace pictor::graph

#endif // PICTOR_HAS_VULKAN
