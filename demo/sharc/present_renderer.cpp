#ifdef PICTOR_HAS_VULKAN

#include "present_renderer.h"

#include "pictor/surface/vulkan_context.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace sharc_demo {

PresentRenderer::~PresentRenderer() {
    shutdown();
}

VkShaderModule PresentRenderer::load_shader_(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return VK_NULL_HANDLE;
    const std::streamsize size = file.tellg();
    file.seekg(0);
    std::vector<char> code(static_cast<size_t>(size));
    file.read(code.data(), size);
    if (!file.good() || size == 0) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = static_cast<size_t>(size);
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &ci, nullptr, &mod) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return mod;
}

bool PresentRenderer::initialize(pictor::VulkanContext& vk,
                                 const char* shader_dir, VkBuffer output,
                                 VkDeviceSize output_size) {
    vk_     = &vk;
    device_ = vk.device();

    // ── descriptor: SSBO 1 個 (fragment 読み) ──
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dli{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 1;
    dli.pBindings    = &binding;
    if (vkCreateDescriptorSetLayout(device_, &dli, nullptr, &dsl_) !=
        VK_SUCCESS) {
        return false;
    }

    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                            sizeof(PushParams)};
    VkPipelineLayoutCreateInfo pli{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount         = 1;
    pli.pSetLayouts            = &dsl_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(device_, &pli, nullptr, &layout_) !=
        VK_SUCCESS) {
        shutdown();
        return false;
    }

    // ── graphics pipeline (フルスクリーン三角形、 頂点入力なし) ──
    const std::string dir = shader_dir;
    VkShaderModule vs = load_shader_((dir + "/sharc_present.vert.spv").c_str());
    VkShaderModule fs = load_shader_((dir + "/sharc_present.frag.spv").c_str());
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        std::fprintf(stderr, "[present] shader not found in %s\n",
                     shader_dir);
        if (vs) vkDestroyShaderModule(device_, vs, nullptr);
        if (fs) vkDestroyShaderModule(device_, fs, nullptr);
        shutdown();
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr,
                 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr,
                 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};

    VkPipelineVertexInputStateCreateInfo vin{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
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
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments    = &blend;
    const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                          VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dyn_states;

    VkGraphicsPipelineCreateInfo gpi{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount          = 2;
    gpi.pStages             = stages;
    gpi.pVertexInputState   = &vin;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState      = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState   = &ms;
    gpi.pColorBlendState    = &cb;
    gpi.pDynamicState       = &dyn;
    gpi.layout              = layout_;
    gpi.renderPass          = vk.default_render_pass();
    gpi.subpass             = 0;
    const VkResult pr = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1,
                                                  &gpi, nullptr, &pipeline_);
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
    if (pr != VK_SUCCESS) {
        std::fprintf(stderr, "[present] pipeline creation failed\n");
        shutdown();
        return false;
    }

    // ── descriptor pool + set (バッファ固定なので 1 回書き) ──
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets       = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes    = &pool_size;
    if (vkCreateDescriptorPool(device_, &dpi, nullptr, &desc_pool_) !=
        VK_SUCCESS) {
        shutdown();
        return false;
    }
    VkDescriptorSetAllocateInfo dai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool     = desc_pool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts        = &dsl_;
    if (vkAllocateDescriptorSets(device_, &dai, &desc_set_) != VK_SUCCESS) {
        shutdown();
        return false;
    }
    VkDescriptorBufferInfo info{output, 0, output_size};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet          = desc_set_;
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo     = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    initialized_ = true;
    return true;
}

void PresentRenderer::render(VkCommandBuffer cmd, VkExtent2D extent,
                             uint32_t render_w, uint32_t render_h,
                             float exposure, bool albedo_mode) {
    if (!initialized_) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0,
                            1, &desc_set_, 0, nullptr);
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                        static_cast<float>(extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    PushParams pc{render_w, render_h, exposure, albedo_mode ? 1.0f : 0.0f};
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void PresentRenderer::shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        if (pipeline_)  vkDestroyPipeline(device_, pipeline_, nullptr);
        if (layout_)    vkDestroyPipelineLayout(device_, layout_, nullptr);
        if (dsl_)       vkDestroyDescriptorSetLayout(device_, dsl_, nullptr);
        if (desc_pool_) vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
        pipeline_  = VK_NULL_HANDLE;
        layout_    = VK_NULL_HANDLE;
        dsl_       = VK_NULL_HANDLE;
        desc_pool_ = VK_NULL_HANDLE;
        desc_set_  = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
    vk_     = nullptr;
    initialized_ = false;
}

}  // namespace sharc_demo

#endif  // PICTOR_HAS_VULKAN
