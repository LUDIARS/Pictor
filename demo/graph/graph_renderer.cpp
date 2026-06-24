#include "graph_renderer.h"

#ifdef PICTOR_HAS_VULKAN

#include "graph_store.h"
#include "pictor/surface/vulkan_context.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace pictor::graph {

namespace {

inline void unpack_rgba(uint32_t rgba, float out[4]) {
    out[0] = static_cast<float>((rgba >> 24) & 0xFFu) / 255.0f;
    out[1] = static_cast<float>((rgba >> 16) & 0xFFu) / 255.0f;
    out[2] = static_cast<float>((rgba >> 8)  & 0xFFu) / 255.0f;
    out[3] = static_cast<float>( rgba        & 0xFFu) / 255.0f;
}

constexpr float kEdgeThicknessPx = 1.5f;

} // namespace

GraphRenderer::~GraphRenderer() {
    if (initialized_) shutdown();
}

bool GraphRenderer::initialize(VulkanContext& vk_ctx, const char* shader_dir,
                               const GraphStore& store) {
    printf("[Graph] Initializing (%zu nodes, %zu edges)...\n",
           store.node_count(), store.edge_count());
    vk_ctx_ = &vk_ctx;
    device_ = vk_ctx.device();

    if (!build_instance_buffers(store)) {
        fprintf(stderr, "[Graph] Failed to build instance buffers\n");
        return false;
    }
    if (!create_descriptors()) {
        fprintf(stderr, "[Graph] Failed to create descriptors\n");
        return false;
    }

    const std::string node_vert = std::string(shader_dir) + "/graph_node.vert.spv";
    const std::string edge_vert = std::string(shader_dir) + "/graph_edge.vert.spv";
    const std::string flat_frag = std::string(shader_dir) + "/graph_flat.frag.spv";

    if (!create_pipeline(node_vert.c_str(), flat_frag.c_str(), node_pipeline_) ||
        !create_pipeline(edge_vert.c_str(), flat_frag.c_str(), edge_pipeline_)) {
        fprintf(stderr, "[Graph] Failed to create pipelines\n");
        return false;
    }

    initialized_ = true;
    printf("[Graph] Initialization complete\n");
    return true;
}

void GraphRenderer::shutdown() {
    if (!initialized_ && !vk_ctx_) return;
    vkDeviceWaitIdle(device_);

    if (node_pipeline_)   vkDestroyPipeline(device_, node_pipeline_, nullptr);
    if (edge_pipeline_)   vkDestroyPipeline(device_, edge_pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (desc_pool_)       vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
    if (desc_set_layout_) vkDestroyDescriptorSetLayout(device_, desc_set_layout_, nullptr);

    auto destroy_buf = [&](VkBuffer& b, VkDeviceMemory& m) {
        if (b) vkDestroyBuffer(device_, b, nullptr);
        if (m) vkFreeMemory(device_, m, nullptr);
        b = VK_NULL_HANDLE; m = VK_NULL_HANDLE;
    };
    destroy_buf(node_buffer_, node_memory_);
    destroy_buf(edge_buffer_, edge_memory_);

    node_pipeline_   = VK_NULL_HANDLE;
    edge_pipeline_   = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    desc_pool_       = VK_NULL_HANDLE;
    desc_set_layout_ = VK_NULL_HANDLE;
    initialized_     = false;
}

// ─── Instance buffers ────────────────────────────────────────

bool GraphRenderer::build_instance_buffers(const GraphStore& store) {
    const auto& nodes = store.nodes();
    const auto& edges = store.edges();
    node_count_ = static_cast<uint32_t>(store.node_count());
    edge_count_ = static_cast<uint32_t>(store.edge_count());

    // Nodes → flat AoS instance records.
    std::vector<NodeInstance> node_inst(node_count_);
    for (uint32_t i = 0; i < node_count_; ++i) {
        NodeInstance& d = node_inst[i];
        d.rect[0] = nodes.cx[i];
        d.rect[1] = nodes.cy[i];
        d.rect[2] = nodes.w[i];
        d.rect[3] = nodes.h[i];
        unpack_rgba(nodes.rgba[i], d.color);
    }

    // Edges → endpoints looked up from node centers (cold path, once).
    std::vector<EdgeInstance> edge_inst(edge_count_);
    for (uint32_t i = 0; i < edge_count_; ++i) {
        const uint32_t a = edges.from[i];
        const uint32_t b = edges.to[i];
        EdgeInstance& d = edge_inst[i];
        d.ends[0] = nodes.cx[a];
        d.ends[1] = nodes.cy[a];
        d.ends[2] = nodes.cx[b];
        d.ends[3] = nodes.cy[b];
        d.color[0] = 0.45f; d.color[1] = 0.50f; d.color[2] = 0.58f; // muted grey-blue
        d.color[3] = kEdgeThicknessPx;
    }

    // Vulkan disallows zero-size buffers; keep a 1-record floor so binding is valid.
    const VkDeviceSize node_bytes =
        std::max<VkDeviceSize>(1, node_count_) * sizeof(NodeInstance);
    const VkDeviceSize edge_bytes =
        std::max<VkDeviceSize>(1, edge_count_) * sizeof(EdgeInstance);

    if (!create_storage_buffer(node_bytes, node_inst.empty() ? nullptr : node_inst.data(),
                               node_buffer_, node_memory_))
        return false;
    if (!create_storage_buffer(edge_bytes, edge_inst.empty() ? nullptr : edge_inst.data(),
                               edge_buffer_, edge_memory_))
        return false;
    return true;
}

bool GraphRenderer::create_storage_buffer(VkDeviceSize size, const void* data,
                                          VkBuffer& buf, VkDeviceMemory& mem) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size  = size;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    if (vkCreateBuffer(device_, &info, nullptr, &buf) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, buf, &req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = find_memory_type(
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(device_, &alloc, nullptr, &mem) != VK_SUCCESS) {
        vkDestroyBuffer(device_, buf, nullptr);
        buf = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(device_, buf, mem, 0);

    if (data) {
        void* mapped = nullptr;
        vkMapMemory(device_, mem, 0, size, 0, &mapped);
        std::memcpy(mapped, data, static_cast<size_t>(size));
        vkUnmapMemory(device_, mem);
    }
    return true;
}

// ─── Descriptors ─────────────────────────────────────────────

bool GraphRenderer::create_descriptors() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 1;
    layout_info.pBindings    = &binding;
    if (vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &desc_set_layout_) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize pool_size{};
    pool_size.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 2;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets       = 2;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes    = &pool_size;
    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &desc_pool_) != VK_SUCCESS)
        return false;

    auto alloc_set = [&](VkDescriptorSet& out) -> bool {
        VkDescriptorSetAllocateInfo a{};
        a.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        a.descriptorPool     = desc_pool_;
        a.descriptorSetCount = 1;
        a.pSetLayouts        = &desc_set_layout_;
        return vkAllocateDescriptorSets(device_, &a, &out) == VK_SUCCESS;
    };
    if (!alloc_set(node_set_) || !alloc_set(edge_set_))
        return false;

    bind_storage_descriptor(node_set_, node_buffer_, VK_WHOLE_SIZE);
    bind_storage_descriptor(edge_set_, edge_buffer_, VK_WHOLE_SIZE);
    return true;
}

void GraphRenderer::bind_storage_descriptor(VkDescriptorSet set, VkBuffer buf,
                                            VkDeviceSize range) {
    VkDescriptorBufferInfo info{};
    info.buffer = buf;
    info.offset = 0;
    info.range  = range;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = set;
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo     = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

// ─── Pipeline ────────────────────────────────────────────────

bool GraphRenderer::create_pipeline(const char* vert_spv, const char* frag_spv,
                                    VkPipeline& out) {
    VkShaderModule vert = load_shader(vert_spv);
    VkShaderModule frag = load_shader(frag_spv);
    if (!vert || !frag) {
        if (vert) vkDestroyShaderModule(device_, vert, nullptr);
        if (frag) vkDestroyShaderModule(device_, frag, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    // No vertex buffers: geometry comes from gl_VertexIndex in the shader.
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = VK_CULL_MODE_NONE;
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.blendEnable         = VK_TRUE;
    blend_att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_att.colorBlendOp        = VK_BLEND_OP_ADD;
    blend_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend_att.alphaBlendOp        = VK_BLEND_OP_ADD;
    blend_att.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments    = &blend_att;

    VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dyn_states;

    // Pipeline layout is shared by both pipelines; create it once (lazily).
    if (pipeline_layout_ == VK_NULL_HANDLE) {
        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        push_range.offset     = 0;
        push_range.size       = sizeof(GraphPushConstants);

        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount         = 1;
        layout_info.pSetLayouts            = &desc_set_layout_;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges    = &push_range;
        if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
            vkDestroyShaderModule(device_, vert, nullptr);
            vkDestroyShaderModule(device_, frag, nullptr);
            return false;
        }
    }

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount          = 2;
    pi.pStages             = stages;
    pi.pVertexInputState   = &vertex_input;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState      = &vp;
    pi.pRasterizationState = &raster;
    pi.pMultisampleState   = &ms;
    pi.pDepthStencilState  = &ds;
    pi.pColorBlendState    = &blend;
    pi.pDynamicState       = &dyn;
    pi.layout              = pipeline_layout_;
    pi.renderPass          = vk_ctx_->default_render_pass();
    pi.subpass             = 0;

    VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pi, nullptr, &out);
    vkDestroyShaderModule(device_, vert, nullptr);
    vkDestroyShaderModule(device_, frag, nullptr);

    if (result != VK_SUCCESS) {
        fprintf(stderr, "[Graph] vkCreateGraphicsPipelines failed (%d)\n", result);
        return false;
    }
    return true;
}

// ─── Render ──────────────────────────────────────────────────

void GraphRenderer::render(VkCommandBuffer cmd, VkExtent2D extent,
                           const GraphPushConstants& pc) {
    if (!initialized_) return;

    VkViewport viewport{};
    viewport.width    = static_cast<float>(extent.width);
    viewport.height   = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(GraphPushConstants), &pc);

    // Edges first (under), then nodes on top.
    if (edge_count_ > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, edge_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout_, 0, 1, &edge_set_, 0, nullptr);
        vkCmdDraw(cmd, 6, edge_count_, 0, 0);
    }
    if (node_count_ > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, node_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout_, 0, 1, &node_set_, 0, nullptr);
        vkCmdDraw(cmd, 6, node_count_, 0, 0);
    }
}

// ─── Helpers ─────────────────────────────────────────────────

VkShaderModule GraphRenderer::load_shader(const char* path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        fprintf(stderr, "[Graph] Cannot open shader: %s\n", path);
        return VK_NULL_HANDLE;
    }
    const size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> code(size);
    file.seekg(0);
    file.read(code.data(), static_cast<std::streamsize>(size));

    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = size;
    info.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &info, nullptr, &mod) != VK_SUCCESS) {
        fprintf(stderr, "[Graph] Failed to create shader module: %s\n", path);
    }
    return mod;
}

uint32_t GraphRenderer::find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(vk_ctx_->physical_device(), &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    fprintf(stderr, "[Graph] No suitable memory type\n");
    return 0;
}

} // namespace pictor::graph

#endif // PICTOR_HAS_VULKAN
