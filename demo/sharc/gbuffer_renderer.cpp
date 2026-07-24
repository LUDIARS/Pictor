#ifdef PICTOR_HAS_VULKAN

#include "gbuffer_renderer.h"

#include "pictor/surface/vulkan_context.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace sharc_demo {

namespace {

uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t filter,
                          VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((filter & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return 0;
}

// G-buffer 3 RT のフォーマット (ヘッダのコメントと一致させる)
constexpr VkFormat kRtFormats[3] = {
    VK_FORMAT_R16G16B16A16_SFLOAT,   // RT0 albedo.rgb + AO
    VK_FORMAT_R16G16B16A16_SFLOAT,   // RT1 normal.xyz + roughness
    VK_FORMAT_R32G32_SFLOAT,         // RT2 dist + MFP (dist は精度必須)
};
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

} // namespace

GBufferRenderer::~GBufferRenderer() {
    shutdown();
}

VkShaderModule GBufferRenderer::load_shader_(const char* path) {
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

bool GBufferRenderer::create_image_(Image& out, uint32_t w, uint32_t h,
                                    VkFormat format, VkImageUsageFlags usage,
                                    VkImageAspectFlags aspect) {
    out.format = format;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = format;
    ici.extent        = {w, h, 1};
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &out.image) != VK_SUCCESS)
        return false;
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(device_, out.image, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = find_memory_type(vk_->physical_device(),
                                          mr.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &ai, nullptr, &out.mem) != VK_SUCCESS)
        return false;
    vkBindImageMemory(device_, out.image, out.mem, 0);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image            = out.image;
    vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    vci.format           = format;
    vci.subresourceRange = {aspect, 0, 1, 0, 1};
    return vkCreateImageView(device_, &vci, nullptr, &out.view) == VK_SUCCESS;
}

void GBufferRenderer::destroy_image_(Image& img) {
    if (img.view)  vkDestroyImageView(device_, img.view, nullptr);
    if (img.image) vkDestroyImage(device_, img.image, nullptr);
    if (img.mem)   vkFreeMemory(device_, img.mem, nullptr);
    img = Image{};
}

bool GBufferRenderer::create_pipelines_(const char* shader_dir) {
    const std::string dir = shader_dir;
    VkShaderModule gvs = load_shader_((dir + "/sharc_gbuffer.vert.spv").c_str());
    VkShaderModule gfs = load_shader_((dir + "/sharc_gbuffer.frag.spv").c_str());
    VkShaderModule svs = load_shader_((dir + "/sharc_shadow.vert.spv").c_str());
    if (gvs == VK_NULL_HANDLE || gfs == VK_NULL_HANDLE ||
        svs == VK_NULL_HANDLE) {
        std::fprintf(stderr, "[gbuffer] shader not found in %s\n", shader_dir);
        if (gvs) vkDestroyShaderModule(device_, gvs, nullptr);
        if (gfs) vkDestroyShaderModule(device_, gfs, nullptr);
        if (svs) vkDestroyShaderModule(device_, svs, nullptr);
        return false;
    }

    // ── 共通固定機能 ──
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
    // 板ポリ (オーニング / 葉) が多いシーンなのでカリングなし
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS;
    const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                          VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dyn_states;

    // ── G-buffer パイプライン (3 RT + depth) ──
    VkPipelineShaderStageCreateInfo gstages[2]{};
    gstages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr,
                  0, VK_SHADER_STAGE_VERTEX_BIT, gvs, "main", nullptr};
    gstages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr,
                  0, VK_SHADER_STAGE_FRAGMENT_BIT, gfs, "main", nullptr};
    VkPipelineColorBlendAttachmentState blends[3]{};
    for (auto& b : blends) {
        b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 3;
    cb.pAttachments    = blends;

    VkGraphicsPipelineCreateInfo gpi{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount          = 2;
    gpi.pStages             = gstages;
    gpi.pVertexInputState   = &vin;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState      = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState   = &ms;
    gpi.pDepthStencilState  = &ds;
    gpi.pColorBlendState    = &cb;
    gpi.pDynamicState       = &dyn;
    gpi.layout              = layout_;
    gpi.renderPass          = gbuffer_pass_;
    gpi.subpass             = 0;
    VkResult pr = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpi,
                                            nullptr, &gbuffer_pipeline_);

    // ── シャドウパイプライン (depth のみ、 fragment なし) ──
    VkPipelineShaderStageCreateInfo sstage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
        VK_SHADER_STAGE_VERTEX_BIT, svs, "main", nullptr};
    VkPipelineColorBlendStateCreateInfo cb0{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo spi = gpi;
    spi.stageCount       = 1;
    spi.pStages          = &sstage;
    spi.pColorBlendState = &cb0;
    spi.renderPass       = shadow_pass_;
    VkResult sr = VK_ERROR_UNKNOWN;
    if (pr == VK_SUCCESS) {
        sr = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &spi,
                                       nullptr, &shadow_pipeline_);
    }

    vkDestroyShaderModule(device_, gvs, nullptr);
    vkDestroyShaderModule(device_, gfs, nullptr);
    vkDestroyShaderModule(device_, svs, nullptr);
    if (pr != VK_SUCCESS || sr != VK_SUCCESS) {
        std::fprintf(stderr, "[gbuffer] pipeline creation failed\n");
        return false;
    }
    return true;
}

bool GBufferRenderer::initialize(pictor::VulkanContext& vk,
                                 const char* shader_dir, uint32_t render_w,
                                 uint32_t render_h, uint32_t tri_count,
                                 VkBuffer tris, VkDeviceSize tris_size,
                                 VkBuffer tri_mats, VkDeviceSize tri_mats_size,
                                 VkBuffer tri_ao, VkDeviceSize tri_ao_size,
                                 VkBuffer materials,
                                 VkDeviceSize materials_size,
                                 VkImageView atlas_view,
                                 VkSampler atlas_sampler) {
    vk_        = &vk;
    device_    = vk.device();
    render_w_  = render_w;
    render_h_  = render_h;
    tri_count_ = tri_count;

    // ── 画像 (G-buffer RT×3 + depth + シャドウマップ) ──
    for (int i = 0; i < 3; ++i) {
        if (!create_image_(rt_[i], render_w, render_h, kRtFormats[i],
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT)) {
            shutdown();
            return false;
        }
    }
    if (!create_image_(depth_, render_w, render_h, kDepthFormat,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT) ||
        !create_image_(shadow_, kShadowSize, kShadowSize, kDepthFormat,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT)) {
        shutdown();
        return false;
    }

    // ── render pass: G-buffer (3 color + depth → SHADER_READ_ONLY) ──
    {
        VkAttachmentDescription atts[4]{};
        for (int i = 0; i < 3; ++i) {
            atts[i].format         = kRtFormats[i];
            atts[i].samples        = VK_SAMPLE_COUNT_1_BIT;
            atts[i].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
            atts[i].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
            atts[i].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            atts[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            atts[i].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
            atts[i].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        atts[3].format         = kDepthFormat;
        atts[3].samples        = VK_SAMPLE_COUNT_1_BIT;
        atts[3].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[3].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[3].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[3].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[3].finalLayout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference color_refs[3] = {
            {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        };
        VkAttachmentReference depth_ref{
            3, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount    = 3;
        sub.pColorAttachments       = color_refs;
        sub.pDepthStencilAttachment = &depth_ref;

        // color write → compute read の外部依存 (実行順は record() 側の
        // submit 順で保証されるが、 可視性はここで張る)
        VkSubpassDependency dep{};
        dep.srcSubpass    = 0;
        dep.dstSubpass    = VK_SUBPASS_EXTERNAL;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpi.attachmentCount = 4;
        rpi.pAttachments    = atts;
        rpi.subpassCount    = 1;
        rpi.pSubpasses      = &sub;
        rpi.dependencyCount = 1;
        rpi.pDependencies   = &dep;
        if (vkCreateRenderPass(device_, &rpi, nullptr, &gbuffer_pass_) !=
            VK_SUCCESS) {
            shutdown();
            return false;
        }
        const VkImageView views[4] = {rt_[0].view, rt_[1].view, rt_[2].view,
                                      depth_.view};
        VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbi.renderPass      = gbuffer_pass_;
        fbi.attachmentCount = 4;
        fbi.pAttachments    = views;
        fbi.width           = render_w;
        fbi.height          = render_h;
        fbi.layers          = 1;
        if (vkCreateFramebuffer(device_, &fbi, nullptr, &gbuffer_fb_) !=
            VK_SUCCESS) {
            shutdown();
            return false;
        }
    }

    // ── render pass: シャドウマップ (depth のみ → SHADER_READ_ONLY) ──
    {
        VkAttachmentDescription att{};
        att.format         = kDepthFormat;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference depth_ref{
            0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.pDepthStencilAttachment = &depth_ref;
        VkSubpassDependency dep{};
        dep.srcSubpass    = 0;
        dep.dstSubpass    = VK_SUBPASS_EXTERNAL;
        dep.srcStageMask  = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        dep.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpi.attachmentCount = 1;
        rpi.pAttachments    = &att;
        rpi.subpassCount    = 1;
        rpi.pSubpasses      = &sub;
        rpi.dependencyCount = 1;
        rpi.pDependencies   = &dep;
        if (vkCreateRenderPass(device_, &rpi, nullptr, &shadow_pass_) !=
            VK_SUCCESS) {
            shutdown();
            return false;
        }
        VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbi.renderPass      = shadow_pass_;
        fbi.attachmentCount = 1;
        fbi.pAttachments    = &shadow_.view;
        fbi.width           = kShadowSize;
        fbi.height          = kShadowSize;
        fbi.layers          = 1;
        if (vkCreateFramebuffer(device_, &fbi, nullptr, &shadow_fb_) !=
            VK_SUCCESS) {
            shutdown();
            return false;
        }
    }

    // ── descriptor: SSBO×4 (tris / tri_mat / tri_ao / materials)
    //    + atlas sampler (fragment) ──
    {
        VkDescriptorSetLayoutBinding bindings[5]{};
        for (uint32_t i = 0; i < 4; ++i) {
            bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                           VkShaderStageFlags{VK_SHADER_STAGE_VERTEX_BIT |
                                              VK_SHADER_STAGE_FRAGMENT_BIT},
                           nullptr};
        }
        bindings[4] = {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo dli{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dli.bindingCount = 5;
        dli.pBindings    = bindings;
        if (vkCreateDescriptorSetLayout(device_, &dli, nullptr, &dsl_) !=
            VK_SUCCESS) {
            shutdown();
            return false;
        }
        VkPushConstantRange pcr{
            VkShaderStageFlags{VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT},
            0, sizeof(PushParams)};
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

        VkDescriptorPoolSize sizes[2] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
        };
        VkDescriptorPoolCreateInfo dpi{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets       = 1;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes    = sizes;
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
        if (vkAllocateDescriptorSets(device_, &dai, &desc_set_) !=
            VK_SUCCESS) {
            shutdown();
            return false;
        }

        const VkDescriptorBufferInfo binfos[4] = {
            {tris, 0, tris_size},
            {tri_mats, 0, tri_mats_size},
            {tri_ao, 0, tri_ao_size},
            {materials, 0, materials_size},
        };
        VkDescriptorImageInfo iinfo{atlas_sampler, atlas_view,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet writes[5]{};
        for (uint32_t i = 0; i < 4; ++i) {
            writes[i] = VkWriteDescriptorSet{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet          = desc_set_;
            writes[i].dstBinding      = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo     = &binfos[i];
        }
        writes[4] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[4].dstSet          = desc_set_;
        writes[4].dstBinding      = 4;
        writes[4].descriptorCount = 1;
        writes[4].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[4].pImageInfo      = &iinfo;
        vkUpdateDescriptorSets(device_, 5, writes, 0, nullptr);
    }

    if (!create_pipelines_(shader_dir)) {
        shutdown();
        return false;
    }

    initialized_ = true;
    std::fprintf(stderr,
                 "[gbuffer] ready: %ux%u G-buffer + %ux%u sun shadow map\n",
                 render_w, render_h, kShadowSize, kShadowSize);
    return true;
}

void GBufferRenderer::record(VkCommandBuffer cmd, const float* view_proj16,
                             const float* sun_view_proj16,
                             const float* camera_pos3) {
    if (!initialized_) return;
    const uint32_t vertex_count = tri_count_ * 3;

    // ── 1. 太陽シャドウマップ (~1-2ms 試算) ──
    {
        VkClearValue clear{};
        clear.depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass      = shadow_pass_;
        rp.framebuffer     = shadow_fb_;
        rp.renderArea      = {{0, 0}, {kShadowSize, kShadowSize}};
        rp.clearValueCount = 1;
        rp.pClearValues    = &clear;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          shadow_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                                0, 1, &desc_set_, 0, nullptr);
        VkViewport vp{0.0f, 0.0f, static_cast<float>(kShadowSize),
                      static_cast<float>(kShadowSize), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {kShadowSize, kShadowSize}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        PushParams pc{};
        std::memcpy(pc.view_proj, sun_view_proj16, sizeof(pc.view_proj));
        vkCmdPushConstants(cmd, layout_,
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDraw(cmd, vertex_count, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    // ── 2. G-buffer (~2-3ms 試算 @720p / 284万tri / 1070) ──
    {
        VkClearValue clears[4]{};
        clears[0].color        = {{0.0f, 0.0f, 0.0f, 1.0f}};
        clears[1].color        = {{0.0f, 1.0f, 0.0f, 0.0f}};
        clears[2].color        = {{0.0f, 0.0f, 0.0f, 0.0f}};  // dist 0 = ミス
        clears[3].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass      = gbuffer_pass_;
        rp.framebuffer     = gbuffer_fb_;
        rp.renderArea      = {{0, 0}, {render_w_, render_h_}};
        rp.clearValueCount = 4;
        rp.pClearValues    = clears;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          gbuffer_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                                0, 1, &desc_set_, 0, nullptr);
        VkViewport vp{0.0f, 0.0f, static_cast<float>(render_w_),
                      static_cast<float>(render_h_), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {render_w_, render_h_}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        PushParams pc{};
        std::memcpy(pc.view_proj, view_proj16, sizeof(pc.view_proj));
        pc.camera_pos[0] = camera_pos3[0];
        pc.camera_pos[1] = camera_pos3[1];
        pc.camera_pos[2] = camera_pos3[2];
        pc.camera_pos[3] = 0.0f;
        vkCmdPushConstants(cmd, layout_,
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDraw(cmd, vertex_count, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }
}

void GBufferRenderer::shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        if (gbuffer_pipeline_)
            vkDestroyPipeline(device_, gbuffer_pipeline_, nullptr);
        if (shadow_pipeline_)
            vkDestroyPipeline(device_, shadow_pipeline_, nullptr);
        if (layout_)     vkDestroyPipelineLayout(device_, layout_, nullptr);
        if (dsl_)        vkDestroyDescriptorSetLayout(device_, dsl_, nullptr);
        if (desc_pool_)  vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
        if (gbuffer_fb_) vkDestroyFramebuffer(device_, gbuffer_fb_, nullptr);
        if (shadow_fb_)  vkDestroyFramebuffer(device_, shadow_fb_, nullptr);
        if (gbuffer_pass_) vkDestroyRenderPass(device_, gbuffer_pass_, nullptr);
        if (shadow_pass_)  vkDestroyRenderPass(device_, shadow_pass_, nullptr);
        gbuffer_pipeline_ = VK_NULL_HANDLE;
        shadow_pipeline_  = VK_NULL_HANDLE;
        layout_     = VK_NULL_HANDLE;
        dsl_        = VK_NULL_HANDLE;
        desc_pool_  = VK_NULL_HANDLE;
        desc_set_   = VK_NULL_HANDLE;
        gbuffer_fb_ = VK_NULL_HANDLE;
        shadow_fb_  = VK_NULL_HANDLE;
        gbuffer_pass_ = VK_NULL_HANDLE;
        shadow_pass_  = VK_NULL_HANDLE;
        for (auto& r : rt_) destroy_image_(r);
        destroy_image_(depth_);
        destroy_image_(shadow_);
    }
    device_ = VK_NULL_HANDLE;
    vk_     = nullptr;
    initialized_ = false;
}

}  // namespace sharc_demo

#endif  // PICTOR_HAS_VULKAN
