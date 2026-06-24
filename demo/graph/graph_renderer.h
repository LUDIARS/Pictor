#pragma once

#ifdef PICTOR_HAS_VULKAN

#include <vulkan/vulkan.h>
#include <cstdint>

namespace pictor {

class VulkanContext;

namespace graph {

class GraphStore;

/// Push constants shared by the node and edge pipelines (matches shader layout).
struct GraphPushConstants {
    float proj[16];    // screen px → clip
    float view[16];    // world → screen px (pan/zoom)
    float params[4];   // x = zoom (edge thickness compensation), yzw reserved
};

/// Instanced, DoD graph renderer (Iter relation-graph, Step 1).
///
/// Two pipelines, each driven entirely by gl_VertexIndex + an SSBO of flat
/// instance records — no per-vertex buffers, one draw call per primitive type:
///   - edges: instanced quad stretched along each segment (drawn first, under)
///   - nodes: instanced rounded-box quad (drawn on top)
///
/// Instance data is packed once at initialize() from the SoA GraphStore; the
/// hot render path only binds + issues two vkCmdDraw calls.
class GraphRenderer {
public:
    GraphRenderer() = default;
    ~GraphRenderer();

    GraphRenderer(const GraphRenderer&)            = delete;
    GraphRenderer& operator=(const GraphRenderer&) = delete;

    bool initialize(VulkanContext& vk_ctx, const char* shader_dir, const GraphStore& store);
    void shutdown();

    /// Record both draws into `cmd` (must be inside a render pass).
    void render(VkCommandBuffer cmd, VkExtent2D extent, const GraphPushConstants& pc);

    uint32_t node_count() const { return node_count_; }
    uint32_t edge_count() const { return edge_count_; }
    bool     is_initialized() const { return initialized_; }

private:
    // GPU-packed instance records (std430: vec4 + vec4 = 32 bytes).
    struct NodeInstance { float rect[4];  float color[4]; }; // rect = cx,cy,w,h
    struct EdgeInstance { float ends[4];  float color[4]; }; // ends = ax,ay,bx,by; color.a = thickness(px)

    bool build_instance_buffers(const GraphStore& store);
    bool create_descriptors();
    bool create_pipeline(const char* vert_spv, const char* frag_spv, VkPipeline& out);

    VkShaderModule load_shader(const char* path);
    uint32_t       find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props);
    bool           create_storage_buffer(VkDeviceSize size, const void* data,
                                         VkBuffer& buf, VkDeviceMemory& mem);
    void           bind_storage_descriptor(VkDescriptorSet set, VkBuffer buf, VkDeviceSize range);

    VulkanContext* vk_ctx_ = nullptr;
    VkDevice       device_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout desc_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool      desc_pool_       = VK_NULL_HANDLE;
    VkDescriptorSet       node_set_        = VK_NULL_HANDLE;
    VkDescriptorSet       edge_set_        = VK_NULL_HANDLE;

    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline       node_pipeline_   = VK_NULL_HANDLE;
    VkPipeline       edge_pipeline_   = VK_NULL_HANDLE;

    VkBuffer       node_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory node_memory_ = VK_NULL_HANDLE;
    VkBuffer       edge_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory edge_memory_ = VK_NULL_HANDLE;

    uint32_t node_count_ = 0;
    uint32_t edge_count_ = 0;

    bool initialized_ = false;
};

} // namespace graph
} // namespace pictor

#endif // PICTOR_HAS_VULKAN
