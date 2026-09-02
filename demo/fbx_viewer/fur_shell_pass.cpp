#include "fur_shell_pass.h"

#include "textured_skinned_vertex.h"
#include "vk_buffer_util.h"

#include <cstdio>
#include <cstring>

namespace pictor_fbx_viewer {

void FurShellParams::print() const {
    std::printf("[fur] shells=%u length=%.4f density=%.0f root=%.2f tip=%.2f occl=%.2f "
                "bend=(%.3f,%.3f,%.3f) rim_power=%.1f bounce=%.2f\n",
                shell_count, length, density, root_thickness, tip_thickness, root_occlusion,
                bend[0], bend[1], bend[2], rim_power, ground_bounce);
}

bool FurShellPass::create(const CreateInfo& ci) {
    device_ = ci.device;
    VkDevice d = device_;

    VkShaderModule vs = load_shader_spv(d, ci.shader_dir + "/fur_shell.vert.spv");
    VkShaderModule fs = load_shader_spv(d, ci.shader_dir + "/fur_shell.frag.spv");
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(d, vs, nullptr);
        if (fs) vkDestroyShaderModule(d, fs, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride  = TSV_STRIDE;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[5]{};
    attrs[0].location = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;    attrs[0].offset = TSV_OFFSET_POSITION;
    attrs[1].location = 1; attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;    attrs[1].offset = TSV_OFFSET_NORMAL;
    attrs[2].location = 2; attrs[2].format = VK_FORMAT_R32G32_SFLOAT;       attrs[2].offset = TSV_OFFSET_UV;
    attrs[3].location = 3; attrs[3].format = VK_FORMAT_R32G32B32A32_UINT;   attrs[3].offset = TSV_OFFSET_JOINTS;
    attrs[4].location = 4; attrs[4].format = VK_FORMAT_R32G32B32A32_SFLOAT; attrs[4].offset = TSV_OFFSET_WEIGHTS;

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 5; vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;

    // Shells are opaque, alpha-tested surfaces with depth writes. Culling
    // is left off because FBX
    // winding varies between exporters; inner-facing shells are hidden by
    // the base mesh depth anyway.
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dyn_states;

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size   = sizeof(FurShellPush);

    VkDescriptorSetLayout set_layouts[2] = {ci.scene_layout, ci.tex_layout};
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 2; pl.pSetLayouts = set_layouts;
    pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(d, &pl, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(d, vs, nullptr);
        vkDestroyShaderModule(d, fs, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2; info.pStages = stages;
    info.pVertexInputState   = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState      = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState   = &ms;
    info.pDepthStencilState  = &ds;
    info.pColorBlendState    = &cb;
    info.pDynamicState       = &dyn;
    info.layout     = pipeline_layout_;
    info.renderPass = ci.render_pass;

    VkResult r = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
    vkDestroyShaderModule(d, vs, nullptr);
    vkDestroyShaderModule(d, fs, nullptr);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[fur] pipeline creation failed (%d)\n", static_cast<int>(r));
        vkDestroyPipelineLayout(d, pipeline_layout_, nullptr);
        pipeline_layout_ = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void FurShellPass::destroy() {
    if (!device_) return;
    if (pipeline_)        { vkDestroyPipeline(device_, pipeline_, nullptr);             pipeline_ = VK_NULL_HANDLE; }
    if (pipeline_layout_) { vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr); pipeline_layout_ = VK_NULL_HANDLE; }
    device_ = VK_NULL_HANDLE;
}

void FurShellPass::record(VkCommandBuffer cmd,
                          VkDescriptorSet scene_set,
                          const std::vector<FurShellSubmeshDraw>& submeshes,
                          const FurShellParams& p) const {
    if (!valid() || p.shell_count == 0) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                            0, 1, &scene_set, 0, nullptr);

    FurShellPush push{};
    std::memcpy(push.tip_color, p.tip_color, sizeof(float) * 3);
    std::memcpy(push.rim_color, p.rim_color, sizeof(float) * 3);
    push.rim_color[3] = p.rim_power;
    std::memcpy(push.bend, p.bend, sizeof(float) * 3);
    push.bend[3]  = p.length;
    push.shape[0] = p.density;
    push.shape[1] = p.root_thickness;
    push.shape[2] = p.tip_thickness;
    push.shape[3] = p.root_occlusion;
    push.shell[1] = static_cast<float>(p.shell_count);
    push.shell[2] = p.ground_bounce;

    // Inner shells first so later (outer) shells only write where the
    // strand mask survives.
    for (uint32_t shell = 1; shell <= p.shell_count; ++shell) {
        push.shell[0] = static_cast<float>(shell);
        vkCmdPushConstants(cmd, pipeline_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(FurShellPush), &push);
        for (const FurShellSubmeshDraw& sm : submeshes) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                                    1, 1, &sm.texture_set, 0, nullptr);
            vkCmdDrawIndexed(cmd, sm.index_count, 1, sm.index_start, 0, 0);
        }
    }
}

} // namespace pictor_fbx_viewer
