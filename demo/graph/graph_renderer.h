#pragma once

#ifdef PICTOR_HAS_VULKAN

#include <vulkan/vulkan.h>
#include <cstdint>

#include "graph_instances.h"
#include "vk_util.h"

namespace pictor {

class VulkanContext;

namespace graph {

/// Push constants shared by the node and edge pipelines (matches shader layout).
struct GraphPushConstants {
    float proj[16];    // region-local px -> clip
    float view[16];    // world -> region-local px (pan/zoom)
    float params[4];   // x = zoom (edge thickness compensation), yzw reserved
};

/// Instanced graph renderer. Step 2: draws only the visible subset handed in by
/// the culler each frame, into an arbitrary screen region (so it can live inside
/// a dock leaf), via per-flight dynamic instance buffers.
///
/// Two pipelines, gl_VertexIndex-driven instanced quads, one draw call each:
///   - edges: quad stretched along each segment (drawn first, under)
///   - nodes: box quad (drawn on top)
class GraphRenderer {
public:
    GraphRenderer() = default;
    ~GraphRenderer();

    GraphRenderer(const GraphRenderer&)            = delete;
    GraphRenderer& operator=(const GraphRenderer&) = delete;

    /// `node_capacity` / `edge_capacity` bound the per-frame visible set.
    bool initialize(VulkanContext& vk_ctx, const char* shader_dir,
                    uint32_t node_capacity, uint32_t edge_capacity);
    void shutdown();

    /// Upload the visible instances for `flight` and record both draws into
    /// `cmd` (inside a render pass), clipped to `region`.
    void draw(VkCommandBuffer cmd, VkRect2D region, const GraphPushConstants& pc,
              uint32_t flight,
              const NodeInstance* nodes, uint32_t node_count,
              const EdgeInstance* edges, uint32_t edge_count);

    bool is_initialized() const { return initialized_; }

private:
    VulkanContext* vk_ctx_ = nullptr;
    VkDevice       device_ = VK_NULL_HANDLE;
    uint32_t       flights_ = 1;

    VkDescriptorSetLayout desc_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool      desc_pool_       = VK_NULL_HANDLE;
    VkPipelineLayout      pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline            node_pipeline_   = VK_NULL_HANDLE;
    VkPipeline            edge_pipeline_   = VK_NULL_HANDLE;

    DynamicInstanceBuffers node_buf_;
    DynamicInstanceBuffers edge_buf_;

    bool initialized_ = false;
};

} // namespace graph
} // namespace pictor

#endif // PICTOR_HAS_VULKAN
