#include "pictor/shader/shader_registry.h"

#include <cstdio>
#include <fstream>
#include <utility>

#ifdef PICTOR_HAS_VULKAN
#include "pictor/surface/vulkan_context.h"
#include "pictor/shader/vertex_layout.h"
#endif

namespace pictor {

ShaderRegistry::~ShaderRegistry() { shutdown(); }

ShaderHandle ShaderRegistry::register_shader(CustomShaderDef def) {
    // phase 1 は固定 vert+frag のみ受け付ける。 compute は phase 2。
    if (def.vert_spv.empty() || def.frag_spv.empty()) {
        std::fprintf(stderr,
                     "[shader] register_shader: vert/frag are both required "
                     "(name=%s)\n", def.name.c_str());
        return INVALID_SHADER;
    }
    const ShaderHandle handle = static_cast<ShaderHandle>(shaders_.size());
    shaders_.push_back(std::move(def));
    return handle;
}

const CustomShaderDef* ShaderRegistry::get(ShaderHandle handle) const {
    if (handle == INVALID_SHADER || handle >= shaders_.size()) return nullptr;
    return &shaders_[handle];
}

#ifdef PICTOR_HAS_VULKAN

VkShaderModule ShaderRegistry::load_module_(const std::string& path) const {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "[shader] open failed: %s\n", path.c_str());
        return VK_NULL_HANDLE;
    }
    const size_t sz = static_cast<size_t>(f.tellg());
    std::vector<char> buf(sz);
    f.seekg(0);
    f.read(buf.data(), static_cast<std::streamsize>(sz));

    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = sz;
    ci.pCode    = reinterpret_cast<const uint32_t*>(buf.data());
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &ci, nullptr, &m) != VK_SUCCESS) {
        std::fprintf(stderr, "[shader] vkCreateShaderModule failed: %s\n",
                     path.c_str());
        return VK_NULL_HANDLE;
    }
    return m;
}

bool ShaderRegistry::build_pipelines(VulkanContext& vk,
                                     VkRenderPass     render_pass,
                                     uint32_t         subpass,
                                     VkPipelineLayout pipeline_layout) {
    if (built_) return true;
    if (shaders_.empty()) { built_ = true; return true; }
    if (render_pass == VK_NULL_HANDLE || pipeline_layout == VK_NULL_HANDLE) {
        std::fprintf(stderr, "[shader] build_pipelines: render pass / layout "
                             "must be valid\n");
        return false;
    }

    vk_     = &vk;
    device_ = vk.device();
    pipelines_.assign(shaders_.size(), VK_NULL_HANDLE);

    // PostProcessPipeline::create_pipelines_ と同じ graphics pipeline 生成。
    // 頂点入力は CustomShaderDef::vertex_layout から構築する (§6.2 phase 2)。
    // 空レイアウトのときは頂点入力空 = phase 1 互換 (gl_VertexIndex 駆動)。
    bool all_ok = true;
    for (size_t i = 0; i < shaders_.size(); ++i) {
        const CustomShaderDef& def = shaders_[i];

        VkShaderModule vs = load_module_(def.vert_spv);
        VkShaderModule fs = VK_NULL_HANDLE;
        if (vs) fs = load_module_(def.frag_spv);
        if (!vs || !fs) {
            if (vs) vkDestroyShaderModule(device_, vs, nullptr);
            std::fprintf(stderr, "[shader] pipeline skipped (module load "
                                 "failed): %s\n", def.name.c_str());
            all_ok = false;
            continue;
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

        // 頂点入力: def の VertexLayout を Vulkan 記述へ展開する。
        // 空レイアウトなら binding/attribute 共に 0 = phase 1 互換の
        // 頂点入力空 (gl_VertexIndex 駆動)。 vk_layout は make_create_info
        // が指すバッファの所有者なので、 pipeline 生成まで生存させる。
        const VkVertexInputLayout vk_layout =
            to_vk_vertex_input(def.vertex_layout);
        const VkPipelineVertexInputStateCreateInfo vi =
            vk_layout.make_create_info();

        VkPipelineInputAssemblyStateCreateInfo ia{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo rs{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode    = VK_CULL_MODE_BACK_BIT;
        rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo dss{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        dss.depthTestEnable  = VK_TRUE;
        dss.depthWriteEnable = VK_TRUE;
        dss.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        ba.blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo cb{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments    = &ba;

        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates    = dyn;

        VkGraphicsPipelineCreateInfo gp{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount          = 2;
        gp.pStages             = stages;
        gp.pVertexInputState   = &vi;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState      = &vp;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState   = &ms;
        gp.pDepthStencilState  = &dss;
        gp.pColorBlendState    = &cb;
        gp.pDynamicState       = &dy;
        gp.layout              = pipeline_layout;
        gp.renderPass          = render_pass;
        gp.subpass             = subpass;

        VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp,
                                               nullptr, &pipelines_[i]);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, fs, nullptr);
        if (r != VK_SUCCESS) {
            std::fprintf(stderr, "[shader] vkCreateGraphicsPipelines failed: "
                                 "%s\n", def.name.c_str());
            pipelines_[i] = VK_NULL_HANDLE;
            all_ok = false;
        }
    }

    built_ = true;
    return all_ok;
}

VkPipeline ShaderRegistry::pipeline(ShaderHandle handle) const {
    if (!built_ || handle == INVALID_SHADER || handle >= pipelines_.size())
        return VK_NULL_HANDLE;
    return pipelines_[handle];
}

#endif // PICTOR_HAS_VULKAN

void ShaderRegistry::shutdown() {
#ifdef PICTOR_HAS_VULKAN
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        for (VkPipeline p : pipelines_) {
            if (p != VK_NULL_HANDLE) vkDestroyPipeline(device_, p, nullptr);
        }
        device_ = VK_NULL_HANDLE;
    }
    pipelines_.clear();
    vk_    = nullptr;
    built_ = false;
#endif
}

} // namespace pictor
