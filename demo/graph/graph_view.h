#pragma once

#ifdef PICTOR_HAS_VULKAN

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

#include "camera2d.h"
#include "graph_instances.h"
#include "graph_renderer.h"
#include "graph_store.h"
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

    /// Cull + upload + draw, clipped to the widget's bounds.
    void render(VkCommandBuffer cmd, uint32_t flight);

    uint32_t visible_nodes() const { return last_vis_nodes_; }
    uint32_t visible_edges() const { return last_vis_edges_; }
    uint32_t total_nodes()   const { return static_cast<uint32_t>(store_.node_count()); }
    uint32_t total_edges()   const { return static_cast<uint32_t>(store_.edge_count()); }
    float    zoom()          const { return camera_.zoom(); }
    uint32_t hovered()       const { return hovered_; }

private:
    void fit_to_bounds();
    void to_world(float win_x, float win_y, float& wx, float& wy) const;

    VkRect2D      bounds_{};
    GraphStore    store_;
    SpatialGrid   grid_;
    Camera2D      camera_;
    GraphRenderer renderer_;

    // Per-frame cull scratch (reused; no per-frame allocation after warm-up).
    std::vector<uint32_t>     vis_nodes_;
    std::vector<NodeInstance> node_inst_;
    std::vector<EdgeInstance> edge_inst_;
    std::vector<uint32_t>     vis_stamp_;  // node -> frame stamp (visible this frame)
    uint32_t                  frame_ = 0;

    uint32_t hovered_ = INVALID_NODE;
    float    fit_cx_ = 0.0f, fit_cy_ = 0.0f, fit_zoom_ = 1.0f;
    uint32_t last_vis_nodes_ = 0, last_vis_edges_ = 0;
};

} // namespace graph
} // namespace pictor

#endif // PICTOR_HAS_VULKAN
