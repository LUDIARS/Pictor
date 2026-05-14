#include "pictor/ui/ui_renderer.h"

#ifdef PICTOR_HAS_VULKAN

#include "pictor/surface/vulkan_context.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace pictor {

namespace {
struct UIWidgetPC {
    float rect[4];      // x, y, w, h (px)
    float color[4];     // rgba
    float params[4];    // corner_radius, mode, opacity, _pad
    float nine[4];      // l, t, r, b (px)
    float viewport[4];  // vw, vh, _pad, _pad
};
} // namespace

UIRenderer::~UIRenderer() { shutdown(); }

uint32_t UIRenderer::find_memory_type_(uint32_t filter,
                                       VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(vk_->physical_device(), &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((filter & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return UINT32_MAX;
}

VkShaderModule UIRenderer::load_shader_(const std::string& path) const {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "[ui] shader open failed: %s\n", path.c_str());
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
        std::fprintf(stderr, "[ui] vkCreateShaderModule failed: %s\n", path.c_str());
        return VK_NULL_HANDLE;
    }
    return m;
}

bool UIRenderer::create_white_texture_() {
    const uint8_t px[4] = {255, 255, 255, 255};

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size        = 4;
    bi.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device_, &bi, nullptr, &staging);
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(device_, staging, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = find_memory_type_(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device_, &ai, nullptr, &staging_mem);
    vkBindBufferMemory(device_, staging, staging_mem, 0);
    void* mapped = nullptr;
    vkMapMemory(device_, staging_mem, 0, 4, 0, &mapped);
    std::memcpy(mapped, px, 4);
    vkUnmapMemory(device_, staging_mem);

    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType   = VK_IMAGE_TYPE_2D;
    ic.format      = VK_FORMAT_R8G8B8A8_UNORM;
    ic.extent      = {1, 1, 1};
    ic.mipLevels   = 1;
    ic.arrayLayers = 1;
    ic.samples     = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ic.usage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(device_, &ic, nullptr, &white_image_);
    vkGetImageMemoryRequirements(device_, white_image_, &mr);
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = find_memory_type_(mr.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device_, &ai, nullptr, &white_memory_);
    vkBindImageMemory(device_, white_image_, white_memory_, 0);

    VkCommandBuffer cmd = vk_->begin_single_time_commands();
    auto barrier = [&](VkImageLayout from, VkImageLayout to,
                       VkAccessFlags sa, VkAccessFlags da,
                       VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from; b.newLayout = to;
        b.srcAccessMask = sa; b.dstAccessMask = da;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = white_image_;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {1, 1, 1};
    vkCmdCopyBufferToImage(cmd, staging, white_image_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    vk_->end_single_time_commands(cmd);

    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, staging_mem, nullptr);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = white_image_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(device_, &vi, nullptr, &white_view_) == VK_SUCCESS;
}

bool UIRenderer::create_descriptors_() {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &si, nullptr, &sampler_) != VK_SUCCESS) return false;

    VkDescriptorSetLayoutBinding b{};
    b.binding         = 0;
    b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings    = &b;
    if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &dsl_tex_) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets       = kMaxTextures;
    pi.poolSizeCount = 1;
    pi.pPoolSizes    = &ps;
    if (vkCreateDescriptorPool(device_, &pi, nullptr, &desc_pool_) != VK_SUCCESS)
        return false;

    // texture_id 0 = 内蔵白テクスチャ。
    VkDescriptorSet white = alloc_texture_set_(white_view_);
    if (white == VK_NULL_HANDLE) return false;
    texture_sets_.push_back(white);
    return true;
}

VkDescriptorSet UIRenderer::alloc_texture_set_(VkImageView view) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool     = desc_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &dsl_tex_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device_, &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorImageInfo di{};
    di.sampler     = sampler_;
    di.imageView   = view;
    di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet          = set;
    w.dstBinding      = 0;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo      = &di;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    return set;
}

uint32_t UIRenderer::register_texture(VkImageView view) {
    if (!initialized_ || view == VK_NULL_HANDLE) return 0;
    if (texture_sets_.size() >= kMaxTextures) {
        std::fprintf(stderr, "[ui] texture 上限到達 (%u)\n", kMaxTextures);
        return 0;
    }
    VkDescriptorSet set = alloc_texture_set_(view);
    if (set == VK_NULL_HANDLE) return 0;
    texture_sets_.push_back(set);
    return static_cast<uint32_t>(texture_sets_.size() - 1);
}

bool UIRenderer::create_pipeline_(const std::string& shader_dir, VkRenderPass rp) {
    VkShaderModule vs = load_shader_(shader_dir + "/ui_widget.vert.spv");
    VkShaderModule fs = load_shader_(shader_dir + "/ui_widget.frag.spv");
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(device_, vs, nullptr);
        if (fs) vkDestroyShaderModule(device_, fs, nullptr);
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

    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount  = 1;
    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};

    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    ba.blendEnable         = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.colorBlendOp        = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.alphaBlendOp        = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments    = &ba;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates    = dyn;

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, static_cast<uint32_t>(sizeof(UIWidgetPC))};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount         = 1;
    pl.pSetLayouts            = &dsl_tex_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(device_, &pl, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, fs, nullptr);
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
    gp.pDepthStencilState  = &dss;
    gp.pColorBlendState    = &cb;
    gp.pDynamicState       = &dy;
    gp.layout              = pipeline_layout_;
    gp.renderPass          = rp;
    gp.subpass             = 0;
    VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp,
                                           nullptr, &pipeline_);
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
    return r == VK_SUCCESS;
}

bool UIRenderer::initialize(VulkanContext& vk, const std::string& shader_dir,
                            VkRenderPass target_pass) {
    vk_     = &vk;
    device_ = vk.device();
    if (!create_white_texture_()) {
        std::fprintf(stderr, "[ui] white texture creation failed\n");
        return false;
    }
    if (!create_descriptors_()) {
        std::fprintf(stderr, "[ui] descriptor setup failed\n");
        return false;
    }
    if (!create_pipeline_(shader_dir, target_pass)) {
        std::fprintf(stderr, "[ui] pipeline creation failed\n");
        return false;
    }
    initialized_ = true;
    std::fprintf(stderr, "[ui] UIRenderer ready\n");
    return true;
}

void UIRenderer::record(VkCommandBuffer cmd, VkExtent2D extent,
                        const std::vector<UIDrawCmd>& cmds) {
    if (!initialized_ || cmds.empty()) return;

    VkViewport vp{0.0f, 0.0f, static_cast<float>(extent.width),
                  static_cast<float>(extent.height), 0.0f, 1.0f};
    VkRect2D full{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &full);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    for (const auto& c : cmds) {
        if (c.kind == UIDrawKind::PushClip) {
            VkRect2D sc;
            sc.offset.x = static_cast<int32_t>(c.x < 0 ? 0 : c.x);
            sc.offset.y = static_cast<int32_t>(c.y < 0 ? 0 : c.y);
            int32_t ex = static_cast<int32_t>(c.x + c.w);
            int32_t ey = static_cast<int32_t>(c.y + c.h);
            ex = ex > static_cast<int32_t>(extent.width)  ? extent.width  : ex;
            ey = ey > static_cast<int32_t>(extent.height) ? extent.height : ey;
            sc.extent.width  = (ex > sc.offset.x) ? (ex - sc.offset.x) : 0;
            sc.extent.height = (ey > sc.offset.y) ? (ey - sc.offset.y) : 0;
            vkCmdSetScissor(cmd, 0, 1, &sc);
            continue;
        }
        if (c.kind == UIDrawKind::PopClip) {
            vkCmdSetScissor(cmd, 0, 1, &full);
            continue;
        }
        if (c.kind == UIDrawKind::Text) continue;  // テキストはホストが描画

        float mode = 0.0f;  // Rect
        if (c.kind == UIDrawKind::Image)     mode = 1.0f;
        if (c.kind == UIDrawKind::NineSlice) mode = 2.0f;

        uint32_t tid = c.texture_id;
        if (tid >= texture_sets_.size()) tid = 0;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout_, 0, 1, &texture_sets_[tid], 0, nullptr);

        UIWidgetPC pc{};
        pc.rect[0] = c.x; pc.rect[1] = c.y; pc.rect[2] = c.w; pc.rect[3] = c.h;
        pc.color[0] = c.r; pc.color[1] = c.g; pc.color[2] = c.b; pc.color[3] = c.a;
        pc.params[0] = c.corner_radius;
        pc.params[1] = mode;
        pc.params[2] = c.opacity;
        pc.nine[0] = c.nine[0]; pc.nine[1] = c.nine[1];
        pc.nine[2] = c.nine[2]; pc.nine[3] = c.nine[3];
        pc.viewport[0] = static_cast<float>(extent.width);
        pc.viewport[1] = static_cast<float>(extent.height);
        vkCmdPushConstants(cmd, pipeline_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 4, 1, 0, 0);
    }
}

void UIRenderer::shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (pipeline_)        vkDestroyPipeline(device_, pipeline_, nullptr);
        if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        if (desc_pool_)       vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
        if (dsl_tex_)         vkDestroyDescriptorSetLayout(device_, dsl_tex_, nullptr);
        if (sampler_)         vkDestroySampler(device_, sampler_, nullptr);
        if (white_view_)      vkDestroyImageView(device_, white_view_, nullptr);
        if (white_image_)     vkDestroyImage(device_, white_image_, nullptr);
        if (white_memory_)    vkFreeMemory(device_, white_memory_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
        pipeline_layout_ = VK_NULL_HANDLE;
        desc_pool_ = VK_NULL_HANDLE;
        dsl_tex_ = VK_NULL_HANDLE;
        sampler_ = VK_NULL_HANDLE;
        white_view_ = VK_NULL_HANDLE;
        white_image_ = VK_NULL_HANDLE;
        white_memory_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
    }
    texture_sets_.clear();
    vk_ = nullptr;
    initialized_ = false;
}

} // namespace pictor

#endif // PICTOR_HAS_VULKAN
