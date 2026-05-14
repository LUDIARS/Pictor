#include "pictor/postprocess/postprocess_pipeline.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#ifdef PICTOR_HAS_VULKAN
#include "pictor/surface/vulkan_context.h"
#endif

namespace pictor {

PostProcessPipeline::PostProcessPipeline()  = default;
PostProcessPipeline::~PostProcessPipeline() { shutdown(); }

void PostProcessPipeline::set_config(const PostProcessConfig& config) {
    config_ = config;
}

#ifdef PICTOR_HAS_VULKAN

namespace {
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

bool is_srgb_format(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R8G8B8_SRGB:
        case VK_FORMAT_B8G8R8_SRGB:
            return true;
        default:
            return false;
    }
}

struct ExtractPC { float threshold, soft_threshold, pad0, pad1; };
struct BlurPC    { float dir_x, dir_y, radius, pad0; };
struct GradePC {
    float    bloom_intensity;
    uint32_t tonemap_op;
    float    exposure, gamma, white_point, saturation;
    float    vignette_intensity, vignette_radius, vignette_softness;
    float    lut_intensity, lut_size;
    float    vig_r, vig_g, vig_b;
};
} // namespace

uint32_t PostProcessPipeline::find_memory_type_(uint32_t filter,
                                                VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(vk_->physical_device(), &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((filter & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    // 失敗時は UINT32_MAX を返す。 create_rt_ 等は直後の vkAllocateMemory 失敗で
    // 検知できる (0 を返すと誤ったメモリタイプに静かにバインドしてしまう)。
    return UINT32_MAX;
}

VkShaderModule PostProcessPipeline::load_shader_(const std::string& path) const {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "[postprocess] shader open failed: %s\n", path.c_str());
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
        std::fprintf(stderr, "[postprocess] vkCreateShaderModule failed: %s\n", path.c_str());
        return VK_NULL_HANDLE;
    }
    return m;
}

bool PostProcessPipeline::create_render_passes_(VkFormat output_format) {
    auto make_pass = [&](VkFormat fmt, VkAttachmentLoadOp load,
                         VkImageLayout final_layout, VkRenderPass& out) -> bool {
        VkAttachmentDescription att{};
        att.format         = fmt;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = load;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout    = final_layout;

        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &ref;

        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;

        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1;
        rp.pAttachments    = &att;
        rp.subpassCount    = 1;
        rp.pSubpasses      = &sub;
        rp.dependencyCount = 2;
        rp.pDependencies   = deps;
        return vkCreateRenderPass(device_, &rp, nullptr, &out) == VK_SUCCESS;
    };

    if (!make_pass(kHdrFormat, VK_ATTACHMENT_LOAD_OP_CLEAR,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, rp_scene_))
        return false;
    if (!make_pass(kHdrFormat, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, rp_inter_))
        return false;
    // Output pass leaves the image in COLOR_ATTACHMENT_OPTIMAL so the host
    // can draw a HUD on top with a LOAD render pass before presenting.
    if (!make_pass(output_format, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, rp_output_))
        return false;
    return true;
}

bool PostProcessPipeline::create_rt_(RenderTarget& rt, VkFormat fmt, VkRenderPass rp) {
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType   = VK_IMAGE_TYPE_2D;
    ic.format      = fmt;
    ic.extent      = {extent_.width, extent_.height, 1};
    ic.mipLevels   = 1;
    ic.arrayLayers = 1;
    ic.samples     = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ic.usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ic, nullptr, &rt.image) != VK_SUCCESS) return false;

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(device_, rt.image, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = find_memory_type_(mr.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &ai, nullptr, &rt.memory) != VK_SUCCESS) return false;
    vkBindImageMemory(device_, rt.image, rt.memory, 0);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = rt.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &vi, nullptr, &rt.view) != VK_SUCCESS) return false;

    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass      = rp;
    fi.attachmentCount = 1;
    fi.pAttachments    = &rt.view;
    fi.width           = extent_.width;
    fi.height          = extent_.height;
    fi.layers          = 1;
    return vkCreateFramebuffer(device_, &fi, nullptr, &rt.fb) == VK_SUCCESS;
}

bool PostProcessPipeline::create_targets_() {
    if (!create_rt_(scene_, kHdrFormat, rp_scene_)) return false;
    if (!create_rt_(ping_,  kHdrFormat, rp_inter_)) return false;
    if (!create_rt_(pong_,  kHdrFormat, rp_inter_)) return false;
    fb_scene_ = scene_.fb;
    return true;
}

bool PostProcessPipeline::create_samplers_() {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod       = 1.0f;
    return vkCreateSampler(device_, &si, nullptr, &sampler_) == VK_SUCCESS;
}

bool PostProcessPipeline::upload_lut_(const unsigned char* rgba, int w, int h) {
    const bool have = (rgba != nullptr && w > 0 && h > 0);

    // 1x1 white fallback so the descriptor stays valid; the shader skips
    // the LUT via lut_intensity == 0 when no real LUT is present.
    std::vector<uint8_t> neutral;
    if (!have) {
        w = 1; h = 1;
        neutral = {255, 255, 255, 255};
    }
    const uint8_t* src = have ? rgba : neutral.data();
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;

    // staging buffer
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size  = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
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
    vkMapMemory(device_, staging_mem, 0, bytes, 0, &mapped);
    std::memcpy(mapped, src, static_cast<size_t>(bytes));
    vkUnmapMemory(device_, staging_mem);

    // device-local image (UNORM — the LUT values are the target, no sRGB decode)
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType   = VK_IMAGE_TYPE_2D;
    ic.format      = VK_FORMAT_R8G8B8A8_UNORM;
    ic.extent      = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    ic.mipLevels   = 1;
    ic.arrayLayers = 1;
    ic.samples     = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ic.usage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(device_, &ic, nullptr, &lut_.image);
    vkGetImageMemoryRequirements(device_, lut_.image, &mr);
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = find_memory_type_(mr.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device_, &ai, nullptr, &lut_.memory);
    vkBindImageMemory(device_, lut_.image, lut_.memory, 0);

    VkCommandBuffer cmd = vk_->begin_single_time_commands();
    auto barrier = [&](VkImageLayout from, VkImageLayout to,
                       VkAccessFlags srcA, VkAccessFlags dstA,
                       VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from;
        b.newLayout = to;
        b.srcAccessMask = srcA;
        b.dstAccessMask = dstA;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = lut_.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    vkCmdCopyBufferToImage(cmd, staging, lut_.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    vk_->end_single_time_commands(cmd);

    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, staging_mem, nullptr);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = lut_.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(device_, &vi, nullptr, &lut_.view);

    lut_loaded_ = have;
    return true;
}

bool PostProcessPipeline::create_descriptors_() {
    VkDescriptorSetLayoutBinding b{};
    b.binding         = 0;
    b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings    = &b;
    if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &dsl_single_) != VK_SUCCESS)
        return false;

    VkDescriptorSetLayoutBinding gb[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        gb[i].binding         = i;
        gb[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        gb[i].descriptorCount = 1;
        gb[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    li.bindingCount = 3;
    li.pBindings    = gb;
    if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &dsl_grade_) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets       = 4;
    pi.poolSizeCount = 1;
    pi.pPoolSizes    = &ps;
    if (vkCreateDescriptorPool(device_, &pi, nullptr, &desc_pool_) != VK_SUCCESS)
        return false;

    auto alloc = [&](VkDescriptorSetLayout layout, VkDescriptorSet& out) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool     = desc_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &layout;
        return vkAllocateDescriptorSets(device_, &ai, &out) == VK_SUCCESS;
    };
    if (!alloc(dsl_single_, ds_extract_)) return false;
    if (!alloc(dsl_single_, ds_blur_h_))  return false;
    if (!alloc(dsl_single_, ds_blur_v_))  return false;
    if (!alloc(dsl_grade_,  ds_grade_))   return false;

    write_descriptor_sets_();
    return true;
}

void PostProcessPipeline::write_descriptor_sets_() {
    // ターゲットの image view を descriptor set に書く。 resize 時にも
    // (view が作り直されるので) 再度呼ぶ。 set 自体は再 alloc 不要。
    auto write1 = [&](VkDescriptorSet set, uint32_t binding, VkImageView view) {
        VkDescriptorImageInfo ii{};
        ii.sampler     = sampler_;
        ii.imageView   = view;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet          = set;
        w.dstBinding      = binding;
        w.descriptorCount = 1;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo      = &ii;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    };
    write1(ds_extract_, 0, scene_.view);
    write1(ds_blur_h_,  0, ping_.view);
    write1(ds_blur_v_,  0, pong_.view);
    write1(ds_grade_,   0, scene_.view);  // scene HDR
    write1(ds_grade_,   1, ping_.view);   // bloom result (after blur V)
    write1(ds_grade_,   2, lut_.view);    // LUT strip
}

bool PostProcessPipeline::create_pipelines_(const std::string& shader_dir) {
    VkShaderModule vs = load_shader_(shader_dir + "/fullscreen_quad.vert.spv");
    if (!vs) return false;

    auto make_pipeline = [&](const std::string& frag_spv, VkRenderPass rp,
                             VkPipelineLayout layout, VkPipeline& out) -> bool {
        VkShaderModule fs = load_shader_(shader_dir + "/" + frag_spv);
        if (!fs) return false;

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
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

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
        ba.blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo cb{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments    = &ba;

        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates    = dyn;

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
        gp.layout              = layout;
        gp.renderPass          = rp;
        gp.subpass             = 0;
        VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp,
                                               nullptr, &out);
        vkDestroyShaderModule(device_, fs, nullptr);
        return r == VK_SUCCESS;
    };

    // single-texture layout (extract / blur), 16-byte fragment push range
    VkPushConstantRange pr_single{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount         = 1;
    pl.pSetLayouts            = &dsl_single_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges    = &pr_single;
    if (vkCreatePipelineLayout(device_, &pl, nullptr, &pl_single_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vs, nullptr);
        return false;
    }

    // grade layout (3 textures), 56-byte fragment push range
    VkPushConstantRange pr_grade{VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                 static_cast<uint32_t>(sizeof(GradePC))};
    pl.setLayoutCount         = 1;
    pl.pSetLayouts            = &dsl_grade_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges    = &pr_grade;
    if (vkCreatePipelineLayout(device_, &pl, nullptr, &pl_grade_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vs, nullptr);
        return false;
    }

    bool ok = make_pipeline("bloom_extract.frag.spv", rp_inter_,  pl_single_, pipe_extract_)
           && make_pipeline("bloom_blur.frag.spv",    rp_inter_,  pl_single_, pipe_blur_)
           && make_pipeline("color_grade.frag.spv",   rp_output_, pl_grade_,  pipe_grade_);
    vkDestroyShaderModule(device_, vs, nullptr);
    return ok;
}

bool PostProcessPipeline::create_output_framebuffers_(
        const std::vector<VkImageView>& views) {
    output_fbs_.resize(views.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < views.size(); ++i) {
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass      = rp_output_;
        fi.attachmentCount = 1;
        fi.pAttachments    = &views[i];
        fi.width           = extent_.width;
        fi.height          = extent_.height;
        fi.layers          = 1;
        if (vkCreateFramebuffer(device_, &fi, nullptr, &output_fbs_[i]) != VK_SUCCESS)
            return false;
    }
    return true;
}

bool PostProcessPipeline::initialize_vulkan(VulkanContext& vk,
                                            const std::string& shader_dir,
                                            uint32_t width, uint32_t height,
                                            VkFormat output_format,
                                            const std::vector<VkImageView>& output_views,
                                            const PostProcessConfig& config,
                                            const unsigned char* lut_rgba,
                                            int lut_w, int lut_h) {
    vk_      = &vk;
    device_  = vk.device();
    width_   = width;
    height_  = height;
    extent_  = {width, height};
    config_  = config;
    output_is_srgb_ = is_srgb_format(output_format);

    // HDR 中間ターゲットのフォーマットが color attachment + sampled に
    // 対応しているか確認する。
    {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(vk.physical_device(), kHdrFormat, &fp);
        const VkFormatFeatureFlags need =
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((fp.optimalTilingFeatures & need) != need) {
            std::fprintf(stderr, "[postprocess] R16G16B16A16_SFLOAT が "
                         "color attachment + sampled に未対応\n");
            return false;
        }
    }

    if (!create_render_passes_(output_format)) {
        std::fprintf(stderr, "[postprocess] render pass creation failed\n");
        return false;
    }
    if (!create_targets_())  { std::fprintf(stderr, "[postprocess] target creation failed\n");  return false; }
    if (!create_samplers_()) { std::fprintf(stderr, "[postprocess] sampler creation failed\n"); return false; }
    if (!upload_lut_(lut_rgba, lut_w, lut_h)) {
        std::fprintf(stderr, "[postprocess] LUT setup failed\n");
        return false;
    }
    if (!create_descriptors_()) { std::fprintf(stderr, "[postprocess] descriptor setup failed\n"); return false; }
    if (!create_pipelines_(shader_dir)) {
        std::fprintf(stderr, "[postprocess] pipeline creation failed\n");
        return false;
    }
    if (!create_output_framebuffers_(output_views)) {
        std::fprintf(stderr, "[postprocess] output framebuffer creation failed\n");
        return false;
    }

    vulkan_ready_ = true;
    std::fprintf(stderr, "[postprocess] ready: %ux%u, lut=%s\n",
                 width, height, lut_loaded_ ? config_.color_grading.lut_path.c_str() : "(none)");
    return true;
}

void PostProcessPipeline::record(VkCommandBuffer cmd, uint32_t output_index,
                                 float /*delta_time*/) {
    if (!vulkan_ready_ || output_index >= output_fbs_.size()) return;

    VkViewport vp{0.0f, 0.0f, static_cast<float>(extent_.width),
                  static_cast<float>(extent_.height), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, extent_};

    auto begin_pass = [&](VkRenderPass rp, VkFramebuffer fb) {
        VkRenderPassBeginInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpi.renderPass      = rp;
        rpi.framebuffer     = fb;
        rpi.renderArea      = {{0, 0}, extent_};
        rpi.clearValueCount = 0;
        vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
    };

    const auto& bloom = config_.bloom;
    const auto& tm    = config_.tone_mapping;
    const auto& vig   = config_.vignette;
    const auto& cg    = config_.color_grading;

    // ── Pass 1: bright-pass extraction → ping_ ──
    begin_pass(rp_inter_, ping_.fb);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_extract_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_single_,
                            0, 1, &ds_extract_, 0, nullptr);
    {
        // disabled bloom → impossibly high threshold so nothing is extracted
        ExtractPC pc{bloom.enabled ? bloom.threshold : 1.0e9f,
                     bloom.soft_threshold, 0.0f, 0.0f};
        vkCmdPushConstants(cmd, pl_single_, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
    }
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    const float radius = bloom.enabled ? bloom.radius : 1.0f;

    // ── Pass 2: horizontal blur → pong_ ──
    begin_pass(rp_inter_, pong_.fb);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_blur_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_single_,
                            0, 1, &ds_blur_h_, 0, nullptr);
    {
        BlurPC pc{1.0f / static_cast<float>(extent_.width), 0.0f, radius, 0.0f};
        vkCmdPushConstants(cmd, pl_single_, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
    }
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    // ── Pass 3: vertical blur → ping_ ──
    begin_pass(rp_inter_, ping_.fb);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_blur_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_single_,
                            0, 1, &ds_blur_v_, 0, nullptr);
    {
        BlurPC pc{0.0f, 1.0f / static_cast<float>(extent_.height), radius, 0.0f};
        vkCmdPushConstants(cmd, pl_single_, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
    }
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    // ── Pass 4: final composite (bloom + tonemap + LUT + vignette) → output ──
    begin_pass(rp_output_, output_fbs_[output_index]);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_grade_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_grade_,
                            0, 1, &ds_grade_, 0, nullptr);
    {
        GradePC pc{};
        pc.bloom_intensity = bloom.enabled ? bloom.intensity : 0.0f;
        pc.tonemap_op      = tm.enabled ? static_cast<uint32_t>(tm.op) : 4u;
        pc.exposure        = tm.enabled ? tm.exposure : 1.0f;
        // sRGB swapchain encodes linear→sRGB in hardware; avoid double gamma.
        pc.gamma           = output_is_srgb_ ? 1.0f : tm.gamma;
        pc.white_point     = tm.white_point;
        pc.saturation      = tm.enabled ? tm.saturation : 1.0f;
        pc.vignette_intensity = vig.enabled ? vig.intensity : 0.0f;
        pc.vignette_radius    = vig.radius;
        pc.vignette_softness  = vig.softness;
        pc.lut_intensity      = (cg.enabled && lut_loaded_) ? cg.lut_intensity : 0.0f;
        pc.lut_size           = static_cast<float>(cg.lut_size);
        pc.vig_r = vig.color[0];
        pc.vig_g = vig.color[1];
        pc.vig_b = vig.color[2];
        vkCmdPushConstants(cmd, pl_grade_, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
    }
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

bool PostProcessPipeline::resize(uint32_t width, uint32_t height,
                                 const std::vector<VkImageView>& output_views) {
    if (!vulkan_ready_) return false;
    if (width == extent_.width && height == extent_.height &&
        output_views.size() == output_fbs_.size())
        return true;  // 変化なし

    vkDeviceWaitIdle(device_);

    // サイズ依存リソースのみ破棄 (render pass / pipeline / pool / sampler / LUT は維持)。
    auto destroy_rt = [&](RenderTarget& rt) {
        if (rt.fb)     vkDestroyFramebuffer(device_, rt.fb, nullptr);
        if (rt.view)   vkDestroyImageView(device_, rt.view, nullptr);
        if (rt.image)  vkDestroyImage(device_, rt.image, nullptr);
        if (rt.memory) vkFreeMemory(device_, rt.memory, nullptr);
        rt = RenderTarget{};
    };
    for (VkFramebuffer fb : output_fbs_)
        if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
    output_fbs_.clear();
    destroy_rt(scene_);
    destroy_rt(ping_);
    destroy_rt(pong_);
    fb_scene_ = VK_NULL_HANDLE;

    width_  = width;
    height_ = height;
    extent_ = {width, height};

    if (!create_targets_()) {
        std::fprintf(stderr, "[postprocess] resize: target creation failed\n");
        vulkan_ready_ = false;
        return false;
    }
    if (!create_output_framebuffers_(output_views)) {
        std::fprintf(stderr, "[postprocess] resize: output framebuffer creation failed\n");
        vulkan_ready_ = false;
        return false;
    }
    // image view が作り直されたので descriptor set を書き直す。
    write_descriptor_sets_();
    return true;
}

#endif // PICTOR_HAS_VULKAN

void PostProcessPipeline::shutdown() {
#ifdef PICTOR_HAS_VULKAN
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        auto destroy_rt = [&](RenderTarget& rt) {
            if (rt.fb)     vkDestroyFramebuffer(device_, rt.fb, nullptr);
            if (rt.view)   vkDestroyImageView(device_, rt.view, nullptr);
            if (rt.image)  vkDestroyImage(device_, rt.image, nullptr);
            if (rt.memory) vkFreeMemory(device_, rt.memory, nullptr);
            rt = RenderTarget{};
        };
        for (VkFramebuffer fb : output_fbs_)
            if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
        output_fbs_.clear();
        destroy_rt(scene_);
        destroy_rt(ping_);
        destroy_rt(pong_);
        fb_scene_ = VK_NULL_HANDLE;
        if (lut_.view)   vkDestroyImageView(device_, lut_.view, nullptr);
        if (lut_.image)  vkDestroyImage(device_, lut_.image, nullptr);
        if (lut_.memory) vkFreeMemory(device_, lut_.memory, nullptr);
        lut_ = Texture{};
        if (pipe_extract_) vkDestroyPipeline(device_, pipe_extract_, nullptr);
        if (pipe_blur_)    vkDestroyPipeline(device_, pipe_blur_, nullptr);
        if (pipe_grade_)   vkDestroyPipeline(device_, pipe_grade_, nullptr);
        if (pl_single_)    vkDestroyPipelineLayout(device_, pl_single_, nullptr);
        if (pl_grade_)     vkDestroyPipelineLayout(device_, pl_grade_, nullptr);
        if (desc_pool_)    vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
        if (dsl_single_)   vkDestroyDescriptorSetLayout(device_, dsl_single_, nullptr);
        if (dsl_grade_)    vkDestroyDescriptorSetLayout(device_, dsl_grade_, nullptr);
        if (sampler_)      vkDestroySampler(device_, sampler_, nullptr);
        if (rp_scene_)     vkDestroyRenderPass(device_, rp_scene_, nullptr);
        if (rp_inter_)     vkDestroyRenderPass(device_, rp_inter_, nullptr);
        if (rp_output_)    vkDestroyRenderPass(device_, rp_output_, nullptr);
        pipe_extract_ = pipe_blur_ = pipe_grade_ = VK_NULL_HANDLE;
        pl_single_ = pl_grade_ = VK_NULL_HANDLE;
        desc_pool_ = VK_NULL_HANDLE;
        dsl_single_ = dsl_grade_ = VK_NULL_HANDLE;
        sampler_ = VK_NULL_HANDLE;
        rp_scene_ = rp_inter_ = rp_output_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
    }
    vk_ = nullptr;
#endif
    vulkan_ready_ = false;
}

} // namespace pictor
