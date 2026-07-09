#include "vk_util.h"

#ifdef PICTOR_HAS_VULKAN

#include "pictor/surface/vulkan_context.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace pictor::graph {

VkShaderModule load_shader_module(VkDevice device, const char* spv_path) {
    std::ifstream file(spv_path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        fprintf(stderr, "[vk_util] Cannot open shader: %s\n", spv_path);
        return VK_NULL_HANDLE;
    }
    const size_t size = static_cast<size_t>(file.tellg());
    // SPIR-V は 4 バイト word 列 (codeSize % 4 == 0 が Vulkan の要求)
    if (size == 0 || size % 4 != 0) {
        fprintf(stderr, "[vk_util] Invalid SPIR-V size (%zu bytes): %s\n", size, spv_path);
        return VK_NULL_HANDLE;
    }
    std::vector<char> code(size);
    file.seekg(0);
    if (!file.read(code.data(), static_cast<std::streamsize>(size))) {
        fprintf(stderr, "[vk_util] Short read on shader: %s\n", spv_path);
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = size;
    info.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &mod) != VK_SUCCESS)
        fprintf(stderr, "[vk_util] Failed to create shader module: %s\n", spv_path);
    return mod;
}

uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t type_filter,
                          VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    fprintf(stderr, "[vk_util] No suitable memory type\n");
    return 0;
}

VkPipeline create_instanced_pipeline(VkDevice device, VkRenderPass render_pass,
                                     VkPipelineLayout layout,
                                     const char* vert_spv, const char* frag_spv) {
    VkShaderModule vert = load_shader_module(device, vert_spv);
    VkShaderModule frag = load_shader_module(device, frag_spv);
    if (!vert || !frag) {
        if (vert) vkDestroyShaderModule(device, vert, nullptr);
        if (frag) vkDestroyShaderModule(device, frag, nullptr);
        return VK_NULL_HANDLE;
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
    pi.layout              = layout;
    pi.renderPass          = render_pass;
    pi.subpass             = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline);
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[vk_util] vkCreateGraphicsPipelines failed (%d)\n", result);
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

// ─── DynamicInstanceBuffers ──────────────────────────────────

bool DynamicInstanceBuffers::create_storage(uint32_t f, VkDeviceSize bytes) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size  = bytes;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (vkCreateBuffer(device_, &info, nullptr, &buffers_[f]) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, buffers_[f], &req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = find_memory_type(
        phys_, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device_, &alloc, nullptr, &memories_[f]) != VK_SUCCESS)
        return false;
    vkBindBufferMemory(device_, buffers_[f], memories_[f], 0);
    vkMapMemory(device_, memories_[f], 0, bytes, 0, &mapped_[f]);

    VkDescriptorBufferInfo bi{};
    bi.buffer = buffers_[f];
    bi.offset = 0;
    bi.range  = VK_WHOLE_SIZE;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = sets_[f];
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo     = &bi;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return true;
}

bool DynamicInstanceBuffers::initialize(VulkanContext& vk_ctx, uint32_t flights,
                                        VkDeviceSize capacity_bytes,
                                        VkDescriptorSetLayout set_layout,
                                        VkDescriptorPool pool) {
    device_   = vk_ctx.device();
    phys_     = vk_ctx.physical_device();
    capacity_ = capacity_bytes;
    buffers_.resize(flights, VK_NULL_HANDLE);
    memories_.resize(flights, VK_NULL_HANDLE);
    mapped_.resize(flights, nullptr);
    sets_.resize(flights, VK_NULL_HANDLE);

    for (uint32_t f = 0; f < flights; ++f) {
        VkDescriptorSetAllocateInfo sa{};
        sa.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        sa.descriptorPool     = pool;
        sa.descriptorSetCount = 1;
        sa.pSetLayouts        = &set_layout;
        if (vkAllocateDescriptorSets(device_, &sa, &sets_[f]) != VK_SUCCESS)
            return false;
        if (!create_storage(f, capacity_bytes))
            return false;
    }
    return true;
}

bool DynamicInstanceBuffers::ensure_capacity(VkDeviceSize bytes) {
    if (bytes <= capacity_) return true;
    // Grow with headroom (1.5x of the request) to amortize repeated expands.
    const VkDeviceSize new_cap = bytes + bytes / 2;
    for (size_t f = 0; f < buffers_.size(); ++f) {
        if (mapped_[f])   vkUnmapMemory(device_, memories_[f]);
        if (buffers_[f])  vkDestroyBuffer(device_, buffers_[f], nullptr);
        if (memories_[f]) vkFreeMemory(device_, memories_[f], nullptr);
        buffers_[f]  = VK_NULL_HANDLE;
        memories_[f] = VK_NULL_HANDLE;
        mapped_[f]   = nullptr;
        if (!create_storage(static_cast<uint32_t>(f), new_cap))
            return false;
    }
    capacity_ = new_cap;
    return true;
}

void DynamicInstanceBuffers::upload(uint32_t flight, const void* data, VkDeviceSize bytes) {
    if (bytes == 0 || data == nullptr) return;
    if (flight >= mapped_.size() || mapped_[flight] == nullptr) return;  // alloc 失敗後
    if (bytes > capacity_) bytes = capacity_;   // never overrun the buffer
    std::memcpy(mapped_[flight], data, static_cast<size_t>(bytes));
}

void DynamicInstanceBuffers::destroy(VkDevice device) {
    for (size_t f = 0; f < buffers_.size(); ++f) {
        if (memories_[f]) vkUnmapMemory(device, memories_[f]);
        if (buffers_[f])  vkDestroyBuffer(device, buffers_[f], nullptr);
        if (memories_[f]) vkFreeMemory(device, memories_[f], nullptr);
    }
    buffers_.clear();
    memories_.clear();
    mapped_.clear();
    sets_.clear();
}

} // namespace pictor::graph

#endif // PICTOR_HAS_VULKAN
