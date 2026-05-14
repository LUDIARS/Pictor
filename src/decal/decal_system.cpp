#include "pictor/decal/decal_system.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#ifdef PICTOR_HAS_VULKAN
#include "pictor/surface/vulkan_context.h"
#endif

namespace pictor {

DecalSystem::~DecalSystem() { shutdown(); }

#ifdef PICTOR_HAS_VULKAN

namespace {
constexpr uint32_t kMaxDecals = 64;

struct DecalPC {
    float inv_decal[16];
    float opacity;
    float angle_fade;
    float depth_fade;
    float _pad0;
    float decal_axis[4];   // xyz = 投影軸 (ワールド空間, 正規化), w 未使用
};

/// 一般 4x4 逆行列 (余因子法)。 行優先配列 in/out。
void mat4_inverse(const float m[16], float out[16]) {
    float inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] +
               m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] -
               m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] +
               m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] -
               m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] -
               m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] +
               m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] -
               m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] +
               m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] +
               m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] -
               m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] +
               m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] -
               m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] -
               m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] +
               m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] -
               m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] +
               m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det > -1e-12f && det < 1e-12f) {
        // 特異 — identity を返す。
        std::memset(out, 0, sizeof(float) * 16);
        out[0] = out[5] = out[10] = out[15] = 1.0f;
        return;
    }
    const float id = 1.0f / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * id;
}

/// column-major 4x4 積。 GLSL の `a * b` 相当。 out = a * b。
void mat4_mul(const float a[16], const float b[16], float out[16]) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = s;
        }
}
} // namespace

uint32_t DecalSystem::find_memory_type_(uint32_t filter,
                                        VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(vk_->physical_device(), &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((filter & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return UINT32_MAX;
}

VkShaderModule DecalSystem::load_shader_(const std::string& path) const {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "[decal] shader open failed: %s\n", path.c_str());
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
        std::fprintf(stderr, "[decal] vkCreateShaderModule failed: %s\n", path.c_str());
        return VK_NULL_HANDLE;
    }
    return m;
}

bool DecalSystem::create_render_pass_(VkFormat color_format) {
    // シーンカラー HDR への LOAD + ブレンド合成パス。 in/out とも
    // SHADER_READ_ONLY (scene pass が残したレイアウト / post-process が次に読む)。
    VkAttachmentDescription att{};
    att.format         = color_format;
    att.samples        = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    att.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments    = &ref;

    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments    = &att;
    rp.subpassCount    = 1;
    rp.pSubpasses      = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies   = deps;
    return vkCreateRenderPass(device_, &rp, nullptr, &render_pass_) == VK_SUCCESS;
}

bool DecalSystem::create_framebuffer_() {
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass      = render_pass_;
    fi.attachmentCount = 1;
    fi.pAttachments    = &scene_color_view_;
    fi.width           = extent_.width;
    fi.height          = extent_.height;
    fi.layers          = 1;
    return vkCreateFramebuffer(device_, &fi, nullptr, &framebuffer_) == VK_SUCCESS;
}

bool DecalSystem::create_descriptors_() {
    // set 0: binding 0 = UBO(inv_view_proj), binding 1 = depth sampler
    VkDescriptorSetLayoutBinding sb[2]{};
    sb[0].binding         = 0;
    sb[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sb[0].descriptorCount = 1;
    sb[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    sb[1].binding         = 1;
    sb[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sb[1].descriptorCount = 1;
    sb[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2;
    li.pBindings    = sb;
    if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &dsl_scene_) != VK_SUCCESS)
        return false;

    // set 1: binding 0 = decal texture sampler
    VkDescriptorSetLayoutBinding db{};
    db.binding         = 0;
    db.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    db.descriptorCount = 1;
    db.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    li.bindingCount = 1;
    li.pBindings    = &db;
    if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &dsl_decal_) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize ps[2]{};
    ps[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ps[0].descriptorCount = 1;
    ps[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps[1].descriptorCount = 1 + kMaxDecals;   // depth + per-decal
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets       = 1 + kMaxDecals;
    pi.poolSizeCount = 2;
    pi.pPoolSizes    = ps;
    if (vkCreateDescriptorPool(device_, &pi, nullptr, &desc_pool_) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool     = desc_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &dsl_scene_;
    if (vkAllocateDescriptorSets(device_, &ai, &ds_scene_) != VK_SUCCESS)
        return false;

    // UBO (inv_view_proj 1 個ぶん)
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size        = sizeof(float) * 16;
    bi.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bi, nullptr, &ubo_buf_) != VK_SUCCESS) return false;
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(device_, ubo_buf_, &mr);
    VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mi.allocationSize  = mr.size;
    mi.memoryTypeIndex = find_memory_type_(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device_, &mi, nullptr, &ubo_mem_) != VK_SUCCESS) return false;
    vkBindBufferMemory(device_, ubo_buf_, ubo_mem_, 0);
    vkMapMemory(device_, ubo_mem_, 0, sizeof(float) * 16, 0, &ubo_mapped_);

    // ds_scene_ を書く (UBO + depth view)。
    VkDescriptorBufferInfo ub{ubo_buf_, 0, sizeof(float) * 16};
    VkDescriptorImageInfo  di{};
    di.sampler     = sampler_;
    di.imageView   = scene_depth_view_;
    di.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w[2]{};
    w[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet          = ds_scene_;
    w[0].dstBinding      = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[0].pBufferInfo     = &ub;
    w[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet          = ds_scene_;
    w[1].dstBinding      = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[1].pImageInfo      = &di;
    vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
    return true;
}

bool DecalSystem::create_pipelines_(const std::string& shader_dir) {
    VkShaderModule vs = load_shader_(shader_dir + "/fullscreen_quad.vert.spv");
    VkShaderModule fs = load_shader_(shader_dir + "/decal.frag.spv");
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
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates    = dyn;

    VkDescriptorSetLayout sets[2] = {dsl_scene_, dsl_decal_};
    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                            static_cast<uint32_t>(sizeof(DecalPC))};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount         = 2;
    pl.pSetLayouts            = sets;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(device_, &pl, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, fs, nullptr);
        return false;
    }

    // ブレンドモードごとに 1 パイプライン (シェーダは共通、 fixed-function の
    // ブレンド状態だけ差し替える)。
    auto make_pipe = [&](const VkPipelineColorBlendAttachmentState& ba,
                         VkPipeline& out) -> bool {
        VkPipelineColorBlendStateCreateInfo cb{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments    = &ba;
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
        gp.renderPass          = render_pass_;
        gp.subpass             = 0;
        return vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp,
                                         nullptr, &out) == VK_SUCCESS;
    };
    const VkColorComponentFlags wmask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState alpha{};
    alpha.colorWriteMask      = wmask;
    alpha.blendEnable         = VK_TRUE;
    alpha.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    alpha.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    alpha.colorBlendOp        = VK_BLEND_OP_ADD;
    alpha.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    alpha.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    alpha.alphaBlendOp        = VK_BLEND_OP_ADD;

    // ADDITIVE: out = src.a * src + dst
    VkPipelineColorBlendAttachmentState additive = alpha;
    additive.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    additive.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    additive.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;

    // MULTIPLY: out = src.rgb * dst + (1 - src.a) * dst
    //   src.a=1 → dst*src.rgb (純乗算)、 src.a=0 → dst (無変化)
    VkPipelineColorBlendAttachmentState multiply = alpha;
    multiply.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    multiply.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    multiply.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    multiply.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;

    bool ok = make_pipe(alpha,    pipeline_alpha_)
           && make_pipe(additive, pipeline_additive_)
           && make_pipe(multiply, pipeline_multiply_);
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
    return ok;
}

VkDescriptorSet DecalSystem::alloc_texture_set_(VkImageView texture) {
    if (texture == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool     = desc_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &dsl_decal_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device_, &ai, &set) != VK_SUCCESS) {
        std::fprintf(stderr, "[decal] descriptor set 上限到達 (max %u)\n", kMaxDecals);
        return VK_NULL_HANDLE;
    }
    VkDescriptorImageInfo di{};
    di.sampler     = sampler_;
    di.imageView   = texture;
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

bool DecalSystem::initialize(VulkanContext& vk, const std::string& shader_dir,
                             VkFormat scene_color_format,
                             VkImageView scene_color_view,
                             VkImageView scene_depth_view,
                             VkExtent2D extent) {
    vk_     = &vk;
    device_ = vk.device();
    extent_ = extent;
    scene_color_view_ = scene_color_view;
    scene_depth_view_ = scene_depth_view;

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &si, nullptr, &sampler_) != VK_SUCCESS) {
        std::fprintf(stderr, "[decal] sampler creation failed\n");
        return false;
    }
    if (!create_render_pass_(scene_color_format)) {
        std::fprintf(stderr, "[decal] render pass creation failed\n");
        return false;
    }
    if (!create_framebuffer_()) {
        std::fprintf(stderr, "[decal] framebuffer creation failed\n");
        return false;
    }
    if (!create_descriptors_()) {
        std::fprintf(stderr, "[decal] descriptor setup failed\n");
        return false;
    }
    if (!create_pipelines_(shader_dir)) {
        std::fprintf(stderr, "[decal] pipeline creation failed\n");
        return false;
    }
    initialized_ = true;
    std::fprintf(stderr, "[decal] ready: %ux%u (max %u decals)\n",
                 extent.width, extent.height, kMaxDecals);
    return true;
}

bool DecalSystem::resize(VkImageView scene_color_view, VkImageView scene_depth_view,
                         VkExtent2D extent) {
    if (!initialized_) return false;
    vkDeviceWaitIdle(device_);
    if (framebuffer_) vkDestroyFramebuffer(device_, framebuffer_, nullptr);
    framebuffer_      = VK_NULL_HANDLE;
    scene_color_view_ = scene_color_view;
    scene_depth_view_ = scene_depth_view;
    extent_           = extent;
    if (!create_framebuffer_()) return false;
    // depth view が変わったので ds_scene_ の binding 1 を書き直す。
    VkDescriptorImageInfo di{};
    di.sampler     = sampler_;
    di.imageView   = scene_depth_view_;
    di.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet          = ds_scene_;
    w.dstBinding      = 1;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo      = &di;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    return true;
}

void DecalSystem::record(VkCommandBuffer cmd, const float view[16],
                         const float proj[16]) {
    if (!initialized_) return;

    // 描画する生存デカールを sort_order 昇順で集める。
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < decals_.size(); ++i)
        if (decals_[i].alive && decals_[i].tex_set != VK_NULL_HANDLE)
            order.push_back(i);
    if (order.empty()) return;  // デカール無し — パス自体スキップ
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return decals_[a].desc.sort_order < decals_[b].desc.sort_order;
    });

    // clip → world の逆行列を作る。 頂点シェーダは proj * view * model * pos
    // (column-vector) なので view_proj = proj * view、 その逆を UBO へ。
    float view_proj[16];
    float inv_view_proj[16];
    mat4_mul(proj, view, view_proj);
    mat4_inverse(view_proj, inv_view_proj);
    std::memcpy(ubo_mapped_, inv_view_proj, sizeof(float) * 16);

    VkRenderPassBeginInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpi.renderPass      = render_pass_;
    rpi.framebuffer     = framebuffer_;
    rpi.renderArea      = {{0, 0}, extent_};
    rpi.clearValueCount = 0;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0.0f, 0.0f, static_cast<float>(extent_.width),
                  static_cast<float>(extent_.height), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, extent_};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                            0, 1, &ds_scene_, 0, nullptr);

    VkPipeline bound = VK_NULL_HANDLE;
    for (uint32_t idx : order) {
        const DecalEntry& e = decals_[idx];
        VkPipeline want = pipeline_alpha_;
        if (e.desc.blend == DecalBlend::Additive)      want = pipeline_additive_;
        else if (e.desc.blend == DecalBlend::Multiply) want = pipeline_multiply_;
        if (want != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
            bound = want;
        }

        DecalPC pc{};
        mat4_inverse(e.desc.world_transform, pc.inv_decal);
        pc.opacity    = e.desc.opacity;
        pc.angle_fade = e.desc.angle_fade;
        pc.depth_fade = e.desc.depth_fade;
        // 投影軸 = OBB のローカル Y 軸 (world_transform 列 1) を正規化。
        const float* m = e.desc.world_transform;
        float ax = m[4], ay = m[5], az = m[6];
        float al = std::sqrt(ax * ax + ay * ay + az * az);
        if (al > 1e-6f) { ax /= al; ay /= al; az /= al; }
        pc.decal_axis[0] = ax; pc.decal_axis[1] = ay; pc.decal_axis[2] = az;

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout_, 1, 1, &e.tex_set, 0, nullptr);
        vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(cmd);
}

#endif // PICTOR_HAS_VULKAN

DecalHandle DecalSystem::add(const DecalDescriptor& d) {
#ifdef PICTOR_HAS_VULKAN
    if (!initialized_) return INVALID_DECAL;
    VkDescriptorSet set = alloc_texture_set_(d.texture);
    if (set == VK_NULL_HANDLE) return INVALID_DECAL;
    uint32_t slot;
    if (!free_slots_.empty()) {
        slot = free_slots_.back();
        free_slots_.pop_back();
    } else {
        slot = static_cast<uint32_t>(decals_.size());
        decals_.emplace_back();
    }
    decals_[slot].desc    = d;
    decals_[slot].tex_set = set;
    decals_[slot].alive   = true;
    return slot + 1;  // 1-based handle
#else
    (void)d; return INVALID_DECAL;
#endif
}

void DecalSystem::update(DecalHandle h, const DecalDescriptor& d) {
#ifdef PICTOR_HAS_VULKAN
    if (h == INVALID_DECAL) return;
    const uint32_t slot = h - 1;
    if (slot >= decals_.size() || !decals_[slot].alive) return;
    DecalEntry& e = decals_[slot];
    // texture が変わったら descriptor set を貼り直す。
    if (d.texture != e.desc.texture) {
        if (e.tex_set) vkFreeDescriptorSets(device_, desc_pool_, 1, &e.tex_set);
        e.tex_set = alloc_texture_set_(d.texture);
    }
    e.desc = d;
#else
    (void)h; (void)d;
#endif
}

void DecalSystem::remove(DecalHandle h) {
#ifdef PICTOR_HAS_VULKAN
    if (h == INVALID_DECAL) return;
    const uint32_t slot = h - 1;
    if (slot >= decals_.size() || !decals_[slot].alive) return;
    DecalEntry& e = decals_[slot];
    if (e.tex_set) {
        vkFreeDescriptorSets(device_, desc_pool_, 1, &e.tex_set);
        e.tex_set = VK_NULL_HANDLE;
    }
    e.alive = false;
    free_slots_.push_back(slot);
#else
    (void)h;
#endif
}

void DecalSystem::clear() {
#ifdef PICTOR_HAS_VULKAN
    for (uint32_t i = 0; i < decals_.size(); ++i)
        if (decals_[i].alive) remove(i + 1);
#endif
}

int DecalSystem::count() const {
#ifdef PICTOR_HAS_VULKAN
    int n = 0;
    for (const auto& e : decals_) if (e.alive) ++n;
    return n;
#else
    return 0;
#endif
}

void DecalSystem::shutdown() {
#ifdef PICTOR_HAS_VULKAN
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (pipeline_alpha_)    vkDestroyPipeline(device_, pipeline_alpha_, nullptr);
        if (pipeline_additive_) vkDestroyPipeline(device_, pipeline_additive_, nullptr);
        if (pipeline_multiply_) vkDestroyPipeline(device_, pipeline_multiply_, nullptr);
        if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        if (desc_pool_)       vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
        if (dsl_scene_)       vkDestroyDescriptorSetLayout(device_, dsl_scene_, nullptr);
        if (dsl_decal_)       vkDestroyDescriptorSetLayout(device_, dsl_decal_, nullptr);
        if (ubo_mem_) {
            vkUnmapMemory(device_, ubo_mem_);
            vkFreeMemory(device_, ubo_mem_, nullptr);
        }
        if (ubo_buf_)     vkDestroyBuffer(device_, ubo_buf_, nullptr);
        if (framebuffer_) vkDestroyFramebuffer(device_, framebuffer_, nullptr);
        if (render_pass_) vkDestroyRenderPass(device_, render_pass_, nullptr);
        if (sampler_)     vkDestroySampler(device_, sampler_, nullptr);
        pipeline_alpha_ = pipeline_additive_ = pipeline_multiply_ = VK_NULL_HANDLE;
        pipeline_layout_ = VK_NULL_HANDLE;
        desc_pool_ = VK_NULL_HANDLE;
        dsl_scene_ = dsl_decal_ = VK_NULL_HANDLE;
        ubo_buf_ = VK_NULL_HANDLE;
        ubo_mem_ = VK_NULL_HANDLE;
        ubo_mapped_ = nullptr;
        framebuffer_ = VK_NULL_HANDLE;
        render_pass_ = VK_NULL_HANDLE;
        sampler_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
    }
    decals_.clear();
    free_slots_.clear();
    vk_ = nullptr;
#endif
    initialized_ = false;
}

} // namespace pictor
