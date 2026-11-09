// Graphics pipeline of VectorTextRenderer: two vertex streams (glyph vertices
// per vertex, VectorTextInstance per instance), premultiplied-free alpha blend.
#include "pictor/vector_text/vector_text_renderer.h"

#ifdef PICTOR_HAS_VULKAN

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <vector>

namespace pictor {

namespace {

VkShaderModule load_shader(VkDevice device, const std::string& path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "[vector_text] shader open failed: %s\n", path.c_str());
        return VK_NULL_HANDLE;
    }
    const size_t size = static_cast<size_t>(f.tellg());
    if (size == 0 || size % 4 != 0) {
        std::fprintf(stderr, "[vector_text] not SPIR-V: %s\n", path.c_str());
        return VK_NULL_HANDLE;
    }
    std::vector<uint32_t> code(size / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size));
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = size;
    ci.pCode    = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
    return module;
}

}  // namespace

bool VectorTextRenderer::create_pipeline_(const std::string& shader_dir, VkRenderPass pass) {
    VkShaderModule vs = load_shader(device_, shader_dir + "/vector_text.vert.spv");
    VkShaderModule fs = load_shader(device_, shader_dir + "/vector_text.frag.spv");
    const auto drop_shaders = [&] {
        if (vs != VK_NULL_HANDLE) vkDestroyShaderModule(device_, vs, nullptr);
        if (fs != VK_NULL_HANDLE) vkDestroyShaderModule(device_, fs, nullptr);
    };
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        drop_shaders();
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

    const VkVertexInputBindingDescription bindings[2] = {
        {0, sizeof(GlyphVertex), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(VectorTextInstance), VK_VERTEX_INPUT_RATE_INSTANCE},
    };
    const VkVertexInputAttributeDescription attributes[6] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(GlyphVertex, x))},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(GlyphVertex, offset_x))},
        {2, 0, VK_FORMAT_R32_SFLOAT, static_cast<uint32_t>(offsetof(GlyphVertex, coverage))},
        {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<uint32_t>(offsetof(VectorTextInstance, m))},
        {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<uint32_t>(offsetof(VectorTextInstance, tx))},
        {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<uint32_t>(offsetof(VectorTextInstance, color))},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount   = 2;
    vi.pVertexBindingDescriptions      = bindings;
    vi.vertexAttributeDescriptionCount = 6;
    vi.pVertexAttributeDescriptions    = attributes;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount  = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;  // the em -> screen map flips y, and rotation is free
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable         = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp        = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp        = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments    = &blend;

    const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates    = dynamic;

    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 2};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges    = &push;
    if (vkCreatePipelineLayout(device_, &pl, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        drop_shaders();
        return false;
    }

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount          = 2;
    gp.pStages             = stages;
    gp.pVertexInputState   = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState      = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState   = &ms;
    gp.pDepthStencilState  = &ds;
    gp.pColorBlendState    = &cb;
    gp.pDynamicState       = &dy;
    gp.layout              = pipeline_layout_;
    gp.renderPass          = pass;
    gp.subpass             = 0;
    const VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_);
    drop_shaders();
    return result == VK_SUCCESS;
}

}  // namespace pictor

#endif  // PICTOR_HAS_VULKAN
