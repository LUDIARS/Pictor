#include "ui_rect_renderer.h"

#ifdef PICTOR_HAS_VULKAN

#include "pictor/surface/vulkan_context.h"

#include <cstdio>
#include <string>

namespace pictor::graph {

UiRectRenderer::~UiRectRenderer() {
    if (initialized_) shutdown();
}

bool UiRectRenderer::initialize(VulkanContext& vk_ctx, const char* shader_dir,
                                uint32_t capacity) {
    vk_ctx_  = &vk_ctx;
    device_  = vk_ctx.device();
    flights_ = vk_ctx.frames_in_flight();
    if (capacity == 0) capacity = 1;

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

    VkDescriptorPoolSize ps{};
    ps.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = flights_;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets       = flights_;
    pi.poolSizeCount = 1;
    pi.pPoolSizes    = &ps;
    if (vkCreateDescriptorPool(device_, &pi, nullptr, &desc_pool_) != VK_SUCCESS)
        return false;

    if (!buf_.initialize(vk_ctx, flights_, VkDeviceSize(capacity) * sizeof(UiRect),
                         desc_set_layout_, desc_pool_))
        return false;

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset     = 0;
    pcr.size       = sizeof(UiPushConstants);
    VkPipelineLayoutCreateInfo pli{};
    pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount         = 1;
    pli.pSetLayouts            = &desc_set_layout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(device_, &pli, nullptr, &pipeline_layout_) != VK_SUCCESS)
        return false;

    const std::string dir  = shader_dir;
    const std::string vert = dir + "/ui_rect.vert.spv";
    const std::string frag = dir + "/graph_flat.frag.spv";
    pipeline_ = create_instanced_pipeline(device_, vk_ctx.default_render_pass(),
                                          pipeline_layout_, vert.c_str(), frag.c_str());
    if (!pipeline_) {
        fprintf(stderr, "[UiRect] Failed to create pipeline\n");
        return false;
    }

    initialized_ = true;
    return true;
}

void UiRectRenderer::shutdown() {
    if (!vk_ctx_) return;
    vkDeviceWaitIdle(device_);
    buf_.destroy(device_);
    if (pipeline_)        vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (desc_pool_)       vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
    if (desc_set_layout_) vkDestroyDescriptorSetLayout(device_, desc_set_layout_, nullptr);
    pipeline_        = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    desc_pool_       = VK_NULL_HANDLE;
    desc_set_layout_ = VK_NULL_HANDLE;
    initialized_     = false;
}

void UiRectRenderer::draw(VkCommandBuffer cmd, VkExtent2D extent, const float proj[16],
                          uint32_t flight, const UiRect* rects, uint32_t count) {
    if (!initialized_ || count == 0) return;

    buf_.upload(flight, rects, VkDeviceSize(count) * sizeof(UiRect));

    VkViewport viewport{};
    viewport.width    = static_cast<float>(extent.width);
    viewport.height   = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor = {{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    UiPushConstants pc{};
    for (int i = 0; i < 16; ++i) pc.proj[i] = proj[i];

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(UiPushConstants), &pc);
    VkDescriptorSet set = buf_.set(flight);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_layout_, 0, 1, &set, 0, nullptr);
    vkCmdDraw(cmd, 6, count, 0, 0);
}

} // namespace pictor::graph

#endif // PICTOR_HAS_VULKAN
