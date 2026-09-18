#include "pictor/xr/reprojection_pass.h"

#ifdef PICTOR_HAS_VULKAN

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace pictor::xr {

namespace {

// シェーダの push constant と同じ並び (std430)。
struct WarpPush {
    float    dst_clip_from_src_ndc[16];
    int32_t  grid_cells[2];
    float    src_size[2];
    float    min_texel_rate;
    float    depth_bias;
};
static_assert(sizeof(WarpPush) == 88, "WarpPush must match the shader push constant block");

constexpr uint32_t kVerticesPerCell = 6;

VkShaderModule load_shader(VkDevice device, const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::fprintf(stderr, "[Pictor][xr] shader not found: %s\n", path.c_str());
        return VK_NULL_HANDLE;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0 || (size % 4) != 0) {
        std::fprintf(stderr, "[Pictor][xr] shader is not valid SPIR-V: %s\n", path.c_str());
        return VK_NULL_HANDLE;
    }
    std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), size);

    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = static_cast<size_t>(size);
    info.pCode    = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        std::fprintf(stderr, "[Pictor][xr] vkCreateShaderModule failed: %s\n", path.c_str());
        return VK_NULL_HANDLE;
    }
    return module;
}

VkSampler create_clamp_sampler(VkDevice device, VkFilter filter) {
    VkSamplerCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter    = filter;
    info.minFilter    = filter;
    info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(device, &info, nullptr, &sampler) != VK_SUCCESS) return VK_NULL_HANDLE;
    return sampler;
}

bool is_valid_source(const ReprojectionSource& s) {
    return s.color != VK_NULL_HANDLE && s.depth != VK_NULL_HANDLE &&
           s.width > 0 && s.height > 0;
}

} // namespace

ReprojectionPass::~ReprojectionPass() { shutdown(); }

bool ReprojectionPass::initialize(const ReprojectionPassDesc& desc) {
    if (is_initialized()) return false;
    if (desc.device == VK_NULL_HANDLE || desc.render_pass == VK_NULL_HANDLE ||
        desc.max_sources == 0 || desc.shader_dir.empty()) {
        std::fprintf(stderr, "[Pictor][xr] ReprojectionPass: invalid desc\n");
        return false;
    }
    device_ = desc.device;

    if (!create_descriptor_objects_(desc.max_sources) || !create_pipeline_(desc)) {
        shutdown();
        return false;
    }
    return true;
}

void ReprojectionPass::shutdown() {
    if (device_ == VK_NULL_HANDLE) return;
    if (pipeline_)        vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    // descriptor set は pool の破棄でまとめて解放される。
    if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    if (set_layout_)      vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (color_sampler_)   vkDestroySampler(device_, color_sampler_, nullptr);
    if (depth_sampler_)   vkDestroySampler(device_, depth_sampler_, nullptr);

    pipeline_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    color_sampler_ = VK_NULL_HANDLE;
    depth_sampler_ = VK_NULL_HANDLE;
    sets_.clear();
    sources_.clear();
    used_slots_ = 0;
    device_ = VK_NULL_HANDLE;
}

bool ReprojectionPass::create_descriptor_objects_(uint32_t max_sources) {
    // 色は線形補間、 深度は最近傍 (輪郭で手前と奥の深度を混ぜると偽の面ができる)。
    color_sampler_ = create_clamp_sampler(device_, VK_FILTER_LINEAR);
    depth_sampler_ = create_clamp_sampler(device_, VK_FILTER_NEAREST);
    if (!color_sampler_ || !depth_sampler_) return false;

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 2;
    layout_info.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &set_layout_) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize pool_size{};
    pool_size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = max_sources * 2;
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets       = max_sources;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes    = &pool_size;
    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS)
        return false;

    std::vector<VkDescriptorSetLayout> layouts(max_sources, set_layout_);
    sets_.assign(max_sources, VK_NULL_HANDLE);
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = descriptor_pool_;
    alloc.descriptorSetCount = max_sources;
    alloc.pSetLayouts        = layouts.data();
    if (vkAllocateDescriptorSets(device_, &alloc, sets_.data()) != VK_SUCCESS) {
        sets_.clear();
        return false;
    }
    sources_.assign(max_sources, ReprojectionSource{});
    return true;
}

bool ReprojectionPass::create_pipeline_(const ReprojectionPassDesc& desc) {
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size       = sizeof(WarpPush);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount         = 1;
    layout_info.pSetLayouts            = &set_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges    = &push;
    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS)
        return false;

    const VkShaderModule vs = load_shader(device_, desc.shader_dir + "/reproject_warp.vert.spv");
    const VkShaderModule fs = load_shader(device_, desc.shader_dir + "/reproject_warp.frag.spv");
    // どちらかの読み込みに失敗しても、 成功した側は必ず解放する。
    auto destroy_modules = [&]() {
        if (vs) vkDestroyShaderModule(device_, vs, nullptr);
        if (fs) vkDestroyShaderModule(device_, fs, nullptr);
    };
    if (!vs || !fs) {
        destroy_modules();
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    // 視点が変わると格子の三角形は裏返ることがある。 面の向きでは捨てない。
    raster.cullMode    = VK_CULL_MODE_NONE;
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable  = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments    = &blend_attachment;

    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates    = dynamic_states;

    VkGraphicsPipelineCreateInfo info{};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount          = 2;
    info.pStages             = stages;
    info.pVertexInputState   = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState      = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState   = &multisample;
    info.pDepthStencilState  = &depth;
    info.pColorBlendState    = &blend;
    info.pDynamicState       = &dynamic;
    info.layout              = pipeline_layout_;
    info.renderPass          = desc.render_pass;
    info.subpass             = desc.subpass;

    const VkResult result =
        vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
    destroy_modules();
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "[Pictor][xr] ReprojectionPass: pipeline creation failed\n");
        pipeline_ = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void ReprojectionPass::write_source_(uint32_t slot, const ReprojectionSource& source) {
    VkDescriptorImageInfo color{};
    color.sampler     = color_sampler_;
    color.imageView   = source.color;
    color.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo depth{};
    depth.sampler     = depth_sampler_;
    depth.imageView   = source.depth;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = sets_[slot];
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    writes[0].pImageInfo = &color;
    writes[1].pImageInfo = &depth;
    vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    sources_[slot] = source;
}

uint32_t ReprojectionPass::register_source(const ReprojectionSource& source) {
    if (!is_initialized() || !is_valid_source(source)) return kInvalidSlot;
    if (used_slots_ >= sets_.size()) {
        std::fprintf(stderr, "[Pictor][xr] ReprojectionPass: no free source slot\n");
        return kInvalidSlot;
    }
    const uint32_t slot = used_slots_++;
    write_source_(slot, source);
    return slot;
}

bool ReprojectionPass::update_source(uint32_t slot, const ReprojectionSource& source) {
    if (!is_initialized() || slot >= used_slots_ || !is_valid_source(source)) return false;
    write_source_(slot, source);
    return true;
}

void ReprojectionPass::record(VkCommandBuffer cmd, uint32_t slot,
                              const float4x4& dst_clip_from_src_ndc,
                              const ReprojectionParams& params) const {
    if (!is_initialized() || slot >= used_slots_) return;
    if (params.dst_width == 0 || params.dst_height == 0 || params.grid_cell_px == 0) return;

    const ReprojectionSource& src = sources_[slot];
    const uint32_t cells_x = (src.width  + params.grid_cell_px - 1) / params.grid_cell_px;
    const uint32_t cells_y = (src.height + params.grid_cell_px - 1) / params.grid_cell_px;

    WarpPush push{};
    std::memcpy(push.dst_clip_from_src_ndc, dst_clip_from_src_ndc.m, sizeof(float) * 16);
    push.grid_cells[0]  = static_cast<int32_t>(cells_x);
    push.grid_cells[1]  = static_cast<int32_t>(cells_y);
    push.src_size[0]    = static_cast<float>(src.width);
    push.src_size[1]    = static_cast<float>(src.height);
    push.min_texel_rate = params.min_texel_rate;
    push.depth_bias     = params.depth_bias;

    VkViewport viewport{};
    viewport.width    = static_cast<float>(params.dst_width);
    viewport.height   = static_cast<float>(params.dst_height);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = {params.dst_width, params.dst_height};

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                            0, 1, &sets_[slot], 0, nullptr);
    vkCmdPushConstants(cmd, pipeline_layout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);
    vkCmdDraw(cmd, cells_x * cells_y * kVerticesPerCell, 1, 0, 0);
}

} // namespace pictor::xr

#endif // PICTOR_HAS_VULKAN
