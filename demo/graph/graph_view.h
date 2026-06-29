#pragma once

#ifdef PICTOR_HAS_VULKAN

#include <vulkan/vulkan.h>
#include <atomic>
#include <cstdint>
#include <thread>
#include <unordered_set>
#include <vector>

#include "pictor/profiler/bitmap_text_renderer.h"
#include "camera2d.h"
#include "data_channel.h"
#include "graph_instances.h"
#include "graph_renderer.h"
#include "graph_store.h"
#include "layout_engine.h"
#include "snippet_cache.h"
#include "spatial_grid.h"

namespace pictor {

class VulkanContext;

namespace graph {

/// The Tier B native graph widget: owns the graph data, spatial index, camera
/// and renderer, and draws itself into a screen rect (a dock leaf). Each frame
/// it culls to the visible subset (O(visible)) and uploads only that to the GPU.
/// Hit-testing (hover) goes through the same spatial grid.
class GraphView {
public:
    bool initialize(VulkanContext& vk_ctx, const char* shader_dir, GraphStore store);
    void shutdown();

    /// Source of children for incremental expand. Not owned; outlives the view.
    void set_data_channel(IDataChannel* channel) { channel_ = channel; }

    /// Assign the screen rect this widget occupies (window pixels).
    void set_bounds(VkRect2D bounds) { bounds_ = bounds; }
    VkRect2D bounds() const { return bounds_; }
    bool contains(float win_x, float win_y) const;

    // Input — coordinates are window pixels; the widget maps them to its rect.
    void pan(float dx, float dy);
    void zoom_at(float win_x, float win_y, float factor);
    void update_hover(float win_x, float win_y);
    void clear_hover() { hovered_ = INVALID_NODE; }
    void reset_view();

    /// Node under a window-space point, or INVALID_NODE. Used to turn a click
    /// (press+release without drag) into a selection / expand.
    NodeHandle pick(float win_x, float win_y) const;

    /// Mark a node selected (highlight only — deliberately does NOT move the
    /// camera, so clicking a node never jumps the view).
    void select(NodeHandle handle) { selected_ = handle; }

    /// Fetch a node's children from the data channel and merge them append-only:
    /// existing ids are reused, new ones spawn from the parent and lerp out to a
    /// fan below it. The camera is left untouched. No-op until the initial layout
    /// has settled, when there is no channel, or when the node was expanded.
    void expand_node(NodeHandle handle);

    /// Cull + upload + draw, clipped to the widget's bounds. Also advances the
    /// layout-in animation and applies a finished worker layout.
    void render(VkCommandBuffer cmd, uint32_t flight);

    /// Draw text overlays for visible nodes via LOD: nothing when small, the
    /// symbol name when mid-size (LABEL LOD), and a lazily-fetched code snippet
    /// card when large (NEAR LOD, LRU-cached). Call after render() within the
    /// frame's text batch (begin/end). Bounded by the visible set + a char budget.
    void draw_labels(BitmapTextRenderer& text);

    uint32_t snippet_cache_size() const { return static_cast<uint32_t>(snippets_.size()); }

    bool layout_pending() const { return !layout_applied_; }
    bool animating()      const { return animating_; }

    uint32_t visible_nodes() const { return last_vis_nodes_; }
    uint32_t visible_edges() const { return last_vis_edges_; }
    uint32_t total_nodes()   const { return static_cast<uint32_t>(store_.node_count()); }
    uint32_t total_edges()   const { return static_cast<uint32_t>(store_.edge_count()); }
    float    zoom()          const { return camera_.zoom(); }
    uint32_t hovered()       const { return hovered_; }

private:
    void fit_to_bounds();
    void to_world(float win_x, float win_y, float& wx, float& wy) const;
    void start_layout_worker();
    void maybe_apply_layout();
    void advance_animation();

    VkRect2D      bounds_{};
    GraphStore    store_;
    SpatialGrid   grid_;
    Camera2D      camera_;
    GraphRenderer renderer_;

    // ── Layout (Sugiyama) computed on a worker thread, animated into place ──
    std::thread        layout_thread_;
    std::atomic<bool>  layout_ready_{false};
    LayoutResult       layout_result_;        // written by worker, read after ready
    bool               layout_applied_ = false;
    std::vector<float> anim_x_, anim_y_;       // animated render positions (lerp -> store)
    bool               animating_ = false;

    // Per-frame cull scratch (reused; no per-frame allocation after warm-up).
    std::vector<uint32_t>     vis_nodes_;
    std::vector<NodeInstance> node_inst_;
    std::vector<EdgeInstance> edge_inst_;
    std::vector<uint32_t>     vis_stamp_;  // node -> frame stamp (visible this frame)
    uint32_t                  frame_ = 0;

    uint32_t hovered_ = INVALID_NODE;
    uint32_t selected_ = INVALID_NODE;
    float    fit_cx_ = 0.0f, fit_cy_ = 0.0f, fit_zoom_ = 1.0f;
    uint32_t last_vis_nodes_ = 0, last_vis_edges_ = 0;

    // ── Incremental expand (Step 6) ──
    IDataChannel*               channel_ = nullptr;  // not owned
    std::unordered_set<uint64_t> expanded_;          // node ids already expanded

    // NEAR-LOD snippet cards: lazily fetched, LRU-bounded.
    SnippetCache snippets_{256};
};

} // namespace graph
} // namespace pictor

#endif // PICTOR_HAS_VULKAN
