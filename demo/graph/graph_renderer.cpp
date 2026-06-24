#include "graph_renderer.h"

#ifdef PICTOR_HAS_VULKAN

#include "pictor/surface/vulkan_context.h"

#include <cstdio>
#include <string>

namespace pictor::graph {

GraphRenderer::~GraphRenderer() {
    if (initialized_) shutdown();
}

bool GraphRenderer::initialize(VulkanContext& vk_ctx, const char* shader_dir,
                               uint32_t node_capacity, uint32_t edge_capacity) {
    vk_ctx_  = &vk_ctx;
    device_  = vk_ctx.device();
    flights_ = vk_ctx.frames_in_flight();
    if (node_capacity == 0) node_capacity = 1;
    if (edge_capacity == 0) edge_capacity = 1;

    // Descriptor layout: one storage buffer in the vertex stage.
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo li{};
    li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 1;
    li.pBindings    = &binding;
    if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &desc_set_layout_) != VK_SUCCESS)
        return false;

    // Pool: node + edge, one set per flight each.
    const uint32_t set_count = flights_ * 2;
    VkDescriptorPoolSize ps{};
    ps.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = set_count;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets       = set_count;
    pi.poolSizeCount = 1;
    pi.pPoolSizes    = &ps;
    if (vkCreateDescriptorPool(device_, &pi, nullptr, &desc_pool_) != VK_SUCCESS)
        return false;

    if (!node_buf_.initialize(vk_ctx, flights_,
                              VkDeviceSize(node_capacity) * sizeof(NodeInstance),
                              desc_set_layout_, desc_pool_))
        return false;
    if (!edge_buf_.initialize(vk_ctx, flights_,
                              VkDeviceSize(edge_capacity) * sizeof(EdgeInstance),
                              desc_set_layout_, desc_pool_))
        return false;

    // Shared pipeline layout (GraphPushConstants, vertex stage).
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset     = 0;
    pcr.size       = sizeof(GraphPushConstants);
    VkPipelineLayoutCreateInfo pli{};
    pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount         = 1;
    pli.pSetLayouts            = &desc_set_layout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(device_, &pli, nullptr, &pipeline_layout_) != VK_SUCCESS)
        return false;

    const std::string dir       = shader_dir;
    const std::string node_vert = dir + "/graph_node.vert.spv";
    const std::string edge_vert = dir + "/graph_edge.vert.spv";
    const std::string flat_frag = dir + "/graph_flat.frag.spv";

    node_pipeline_ = create_instanced_pipeline(device_, vk_ctx.default_render_pass(),
                                               pipeline_layout_, node_vert.c_str(), flat_frag.c_str());
    edge_pipeline_ = create_instanced_pipeline(device_, vk_ctx.default_render_pass(),
                                               pipeline_layout_, edge_vert.c_str(), flat_frag.c_str());
    if (!node_pipeline_ || !edge_pipeline_) {
        fprintf(stderr, "[Graph] Failed to create pipelines\n");
        return false;
    }

    initialized_ = true;
    printf("[Graph] Renderer ready (flights=%u, node_cap=%u, edge_cap=%u)\n",
           flights_, node_capacity, edge_capacity);
    return true;
}

void GraphRenderer::shutdown() {
    if (!vk_ctx_) return;
    vkDeviceWaitIdle(device_);

    node_buf_.destroy(device_);
    edge_buf_.destroy(device_);

    if (node_pipeline_)   vkDestroyPipeline(device_, node_pipeline_, nullptr);
    if (edge_pipeline_)   vkDestroyPipeline(device_, edge_pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (desc_pool_)       vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
    if (desc_set_layout_) vkDestroyDescriptorSetLayout(device_, desc_set_layout_, nullptr);

    node_pipeline_   = VK_NULL_HANDLE;
    edge_pipeline_   = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    desc_pool_       = VK_NULL_HANDLE;
    desc_set_layout_ = VK_NULL_HANDLE;
    initialized_     = false;
}

void GraphRenderer::draw(VkCommandBuffer cmd, VkRect2D region, const GraphPushConstants& pc,
                         uint32_t flight,
                         const NodeInstance* nodes, uint32_t node_count,
                         const EdgeInstance* edges, uint32_t edge_count) {
    if (!initialized_) return;

    node_buf_.upload(flight, nodes, VkDeviceSize(node_count) * sizeof(NodeInstance));
    edge_buf_.upload(flight, edges, VkDeviceSize(edge_count) * sizeof(EdgeInstance));

    VkViewport viewport{};
    viewport.x        = static_cast<float>(region.offset.x);
    viewport.y        = static_cast<float>(region.offset.y);
    viewport.width    = static_cast<float>(region.extent.width);
    viewport.height   = static_cast<float>(region.extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &region);

    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(GraphPushConstants), &pc);

    if (edge_count > 0) {
        VkDescriptorSet set = edge_buf_.set(flight);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, edge_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout_, 0, 1, &set, 0, nullptr);
        vkCmdDraw(cmd, 6, edge_count, 0, 0);
    }
    if (node_count > 0) {
        VkDescriptorSet set = node_buf_.set(flight);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, node_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout_, 0, 1, &set, 0, nullptr);
        vkCmdDraw(cmd, 6, node_count, 0, 0);
    }
}

} // namespace pictor::graph

#endif // PICTOR_HAS_VULKAN
