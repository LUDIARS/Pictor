#include "pictor/postprocess/postprocess_pipeline.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#ifdef PICTOR_HAS_VULKAN
#include "pictor/surface/vulkan_context.h"
#include "pictor/shader/vertex_layout.h"
#include "pictor/shader/graphics_pipeline_builder.h"
#endif

namespace pictor {

PostProcessPipeline::PostProcessPipeline()  = default;
PostProcessPipeline::~PostProcessPipeline() { shutdown(); }

void PostProcessPipeline::set_config(const PostProcessConfig& config) {
    config_ = config;
}

#ifdef PICTOR_HAS_VULKAN

namespace {
constexpr VkFormat kHdrFormat   = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

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
} // namespace

VkImageView PostProcessPipeline::scene_color_view() const {
    if (scene_index_ < 0) return VK_NULL_HANDLE;
    return targets_[static_cast<size_t>(scene_index_)].view;
}

VkImageView PostProcessPipeline::scene_depth_view() const {
    if (scene_index_ < 0) return VK_NULL_HANDLE;
    return targets_[static_cast<size_t>(scene_index_)].depth_view;
}

VkImage PostProcessPipeline::scene_color_image() const {
    if (scene_index_ < 0) return VK_NULL_HANDLE;
    return targets_[static_cast<size_t>(scene_index_)].image;
}

VkImage PostProcessPipeline::scene_depth_image() const {
    if (scene_index_ < 0) return VK_NULL_HANDLE;
    return targets_[static_cast<size_t>(scene_index_)].depth_image;
}

uint32_t PostProcessPipeline::find_memory_type_(uint32_t filter,
                                                VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(vk_->physical_device(), &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((filter & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    // 失敗時は UINT32_MAX を返す。 直後の vkAllocateMemory 失敗で検知できる
    // (0 を返すと誤ったメモリタイプに静かにバインドしてしまう)。
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

// ── scene render pass: color (RGBA16F) + depth (D32_SFLOAT), CLEAR ──────────
//    旧実装の rp_scene_ と同一。 DecalSystem 等が color/depth をサンプルする。
bool PostProcessPipeline::create_scene_render_pass_() {
    VkAttachmentDescription atts[2]{};
    atts[0].format         = kHdrFormat;
    atts[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    atts[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    atts[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    atts[1].format         = kDepthFormat;
    atts[1].samples        = VK_SAMPLE_COUNT_1_BIT;
    atts[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    atts[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount    = 1;
    sub.pColorAttachments       = &colorRef;
    sub.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 2;
    rp.pAttachments    = atts;
    rp.subpassCount    = 1;
    rp.pSubpasses      = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies   = deps;
    return vkCreateRenderPass(device_, &rp, nullptr, &rp_scene_) == VK_SUCCESS;
}

// ── 汎用 intermediate render pass: RGBA16F, DONT_CARE → SHADER_READ_ONLY ────
//    全ての「HDR 中間ターゲットへ書く pass」 が共有する。
//    subpass dependency は「直前 pass の fragment-read / color-write を
//    今 pass の color-write がブロック」 + 「今 pass の color-write を
//    次 pass の fragment-read / color-read がブロック」 の 2 本。
//    pass を何枚挟んでも、 各 pass がこの EXTERNAL↔0 依存を持つため
//    挿入順どおりの read-after-write / write-after-read が成立する。
VkRenderPass PostProcessPipeline::get_or_create_inter_render_pass_() {
    if (rp_inter_ != VK_NULL_HANDLE) return rp_inter_;

    VkAttachmentDescription att{};
    att.format         = kHdrFormat;
    att.samples        = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

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
    if (vkCreateRenderPass(device_, &rp, nullptr, &rp_inter_) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return rp_inter_;
}

// ── 出力 render pass: swapchain format, DONT_CARE → COLOR_ATTACHMENT ────────
//    出力 image を COLOR_ATTACHMENT_OPTIMAL のまま残し、 ホストが HUD を
//    LOAD パスで重ねられるようにする。 旧実装の rp_output_ と同一。
VkRenderPass PostProcessPipeline::get_or_create_output_render_pass_(VkFormat fmt) {
    if (rp_output_ != VK_NULL_HANDLE) return rp_output_;

    VkAttachmentDescription att{};
    att.format         = fmt;
    att.samples        = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

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
    if (vkCreateRenderPass(device_, &rp, nullptr, &rp_output_) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return rp_output_;
}

bool PostProcessPipeline::create_target_(RenderTarget& rt, VkRenderPass rp,
                                         bool with_depth, VkExtent2D extent) {
    rt.fb_render_pass = rp;
    rt.has_depth      = with_depth;
    rt.extent         = extent;

    // ── color image ──
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType   = VK_IMAGE_TYPE_2D;
    ic.format      = kHdrFormat;
    ic.extent      = {extent.width, extent.height, 1};
    ic.mipLevels   = 1;
    ic.arrayLayers = 1;
    ic.samples     = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling      = VK_IMAGE_TILING_OPTIMAL;
    // TRANSFER_SRC: history buffer (`__history:*__`) のコピー元になり得る。
    ic.usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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
    vi.format   = kHdrFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &vi, nullptr, &rt.view) != VK_SUCCESS) return false;

    // ── depth image (scene のみ) ──
    if (with_depth) {
        VkImageCreateInfo dic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        dic.imageType   = VK_IMAGE_TYPE_2D;
        dic.format      = kDepthFormat;
        dic.extent      = {extent.width, extent.height, 1};
        dic.mipLevels   = 1;
        dic.arrayLayers = 1;
        dic.samples     = VK_SAMPLE_COUNT_1_BIT;
        dic.tiling      = VK_IMAGE_TILING_OPTIMAL;
        // SAMPLED も付けて DecalSystem 等が深度をサンプルできるようにする。
        dic.usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT;
        dic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        dic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device_, &dic, nullptr, &rt.depth_image) != VK_SUCCESS)
            return false;
        vkGetImageMemoryRequirements(device_, rt.depth_image, &mr);
        ai.allocationSize  = mr.size;
        ai.memoryTypeIndex = find_memory_type_(mr.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device_, &ai, nullptr, &rt.depth_memory) != VK_SUCCESS)
            return false;
        vkBindImageMemory(device_, rt.depth_image, rt.depth_memory, 0);

        VkImageViewCreateInfo dvi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        dvi.image    = rt.depth_image;
        dvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        dvi.format   = kDepthFormat;
        dvi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &dvi, nullptr, &rt.depth_view) != VK_SUCCESS)
            return false;
    }

    // ── framebuffer ──
    VkImageView fb_atts[2] = {rt.view, rt.depth_view};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass      = rp;
    fi.attachmentCount = with_depth ? 2u : 1u;
    fi.pAttachments    = fb_atts;
    fi.width           = extent.width;
    fi.height          = extent.height;
    fi.layers          = 1;
    return vkCreateFramebuffer(device_, &fi, nullptr, &rt.fb) == VK_SUCCESS;
}

// scene + chain の中間ターゲットを名前付きで確保する。
bool PostProcessPipeline::create_targets_() {
    targets_.clear();
    target_index_.clear();

    VkRenderPass rp_inter = get_or_create_inter_render_pass_();
    if (rp_inter == VK_NULL_HANDLE) return false;

    // index 0: scene HDR ターゲット (color + depth, scene render pass)。
    {
        RenderTarget scene;
        if (!create_target_(scene, rp_scene_, /*with_depth=*/true, extent_))
            return false;
        scene_index_ = static_cast<int32_t>(targets_.size());
        target_index_[kPostProcessSceneTarget] = scene_index_;
        targets_.push_back(scene);
        fb_scene_ = scene.fb;
    }
    // chain が宣言する中間ターゲット (ping / pong …)。 depth 無し。
    // target_divisors 宣言があれば縮小して確保する (mip-chain bloom 用)。
    for (const auto& name : chain_.intermediate_names) {
        const uint32_t div = post_process_target_divisor(chain_, name);
        const VkExtent2D ext = {std::max(extent_.width / div, 1u),
                                std::max(extent_.height / div, 1u)};
        RenderTarget rt;
        if (!create_target_(rt, rp_inter, /*with_depth=*/false, ext))
            return false;
        target_index_[name] = static_cast<int32_t>(targets_.size());
        targets_.push_back(rt);
    }
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
    if (vkCreateSampler(device_, &si, nullptr, &sampler_) != VK_SUCCESS)
        return false;

    // 深度入力 (__depth__) 用。 D32_SFLOAT の LINEAR filter はデバイス保証が
    // ないため NEAREST に倒す。
    si.magFilter  = VK_FILTER_NEAREST;
    si.minFilter  = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    return vkCreateSampler(device_, &si, nullptr, &depth_sampler_) == VK_SUCCESS;
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

// input 数ぶんの COMBINED_IMAGE_SAMPLER binding を持つ descriptor set layout。
// 組み込みチェーンは input 数 1 (extract/blur) と 3 (grade) を使う。
VkDescriptorSetLayout PostProcessPipeline::get_or_create_dsl_(uint32_t input_count) {
    auto it = dsl_by_input_count_.find(input_count);
    if (it != dsl_by_input_count_.end()) return it->second;

    std::vector<VkDescriptorSetLayoutBinding> binds(input_count);
    for (uint32_t i = 0; i < input_count; ++i) {
        binds[i].binding         = i;
        binds[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = input_count;
    li.pBindings    = input_count ? binds.data() : nullptr;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &dsl) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    dsl_by_input_count_[input_count] = dsl;
    return dsl;
}

// (input 数, push サイズ) の組ごとに pipeline layout をキャッシュする。
VkPipelineLayout PostProcessPipeline::get_or_create_layout_(uint32_t input_count,
                                                            uint32_t push_size) {
    const uint64_t key = (static_cast<uint64_t>(input_count) << 32) | push_size;
    for (const auto& kv : layout_cache_)
        if (kv.first == key) return kv.second;

    VkDescriptorSetLayout dsl = get_or_create_dsl_(input_count);
    if (dsl == VK_NULL_HANDLE) return VK_NULL_HANDLE;

    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts    = &dsl;
    VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_size};
    if (push_size > 0) {
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges    = &pr;
    }
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device_, &pl, nullptr, &layout) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    layout_cache_.emplace_back(key, layout);
    return layout;
}

// chain の各 pass ぶんに 1 つ descriptor set を確保する。
bool PostProcessPipeline::create_descriptors_() {
    uint32_t total_images = 0;
    for (const auto& cp : compiled_) total_images += cp.input_count;
    if (total_images == 0) total_images = 1;  // pool は 0 不可

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, total_images};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets       = static_cast<uint32_t>(compiled_.size() ? compiled_.size() : 1);
    pi.poolSizeCount = 1;
    pi.pPoolSizes    = &ps;
    if (vkCreateDescriptorPool(device_, &pi, nullptr, &desc_pool_) != VK_SUCCESS)
        return false;

    for (auto& cp : compiled_) {
        VkDescriptorSetLayout dsl = get_or_create_dsl_(cp.input_count);
        if (dsl == VK_NULL_HANDLE) return false;
        VkDescriptorSetAllocateInfo ai{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool     = desc_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &dsl;
        if (vkAllocateDescriptorSets(device_, &ai, &cp.desc_set) != VK_SUCCESS)
            return false;
    }
    write_descriptor_sets_();
    return true;
}

// pass ごとの入力 image view を descriptor set へ書く。
// resize 時 (view 作り直し時) にも再度呼ぶ — set 自体は再 alloc 不要。
void PostProcessPipeline::write_descriptor_sets_() {
    for (const auto& cp : compiled_) {
        if (cp.desc_set == VK_NULL_HANDLE) continue;
        std::vector<VkDescriptorImageInfo> infos(cp.input_count);
        std::vector<VkWriteDescriptorSet>  writes(cp.input_count);
        for (uint32_t i = 0; i < cp.input_count; ++i) {
            // 入力 index >= 0 は targets_、 -1 は LUT (専用 Texture)、
            // -2 は scene 深度 (__depth__、 NEAREST サンプラ)、
            // -3-n は history_[n] (persistent image)。
            const int32_t idx = cp.input_indices[i];
            if (idx >= 0) {
                infos[i].sampler     = sampler_;
                infos[i].imageView   = targets_[static_cast<size_t>(idx)].view;
                infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            } else if (idx == -1) {
                infos[i].sampler     = sampler_;
                infos[i].imageView   = lut_.view;
                infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            } else if (idx == -2) {
                // scene render pass の finalLayout (DEPTH_STENCIL_READ_ONLY)
                // と一致させる。
                infos[i].sampler     = depth_sampler_;
                infos[i].imageView   = scene_depth_view();
                infos[i].imageLayout =
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            } else {
                const size_t h = static_cast<size_t>(-3 - idx);
                infos[i].sampler     = sampler_;
                infos[i].imageView   = history_[h].tex.view;
                infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }

            writes[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet          = cp.desc_set;
            writes[i].dstBinding      = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo      = &infos[i];
        }
        if (!writes.empty())
            vkUpdateDescriptorSets(device_,
                                   static_cast<uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
    }
}

// chain の各 pass ぶんに graphics pipeline を生成する。
// graphics pipeline 生成は共通ヘルパー `build_graphics_pipeline()` に委譲
// する (§6.3 項目6 — ShaderRegistry との重複統合)。 post-process pass は
// fullscreen triangle なのでカリング無し / 深度テスト無し / 頂点入力空
// (gl_VertexIndex 駆動)。
bool PostProcessPipeline::create_pipelines_() {
    bool all_ok = true;
    for (auto& cp : compiled_) {
        // chain の対応する pass を名前で引く。
        const PostProcessPassDef* def = nullptr;
        for (const auto& p : chain_.passes)
            if (p.name == cp.name) { def = &p; break; }
        if (!def) { all_ok = false; continue; }

        VkShaderModule vs = load_shader_(def->vert_spv);
        VkShaderModule fs = VK_NULL_HANDLE;
        if (vs) fs = load_shader_(def->frag_spv);
        if (!vs || !fs) {
            if (vs) vkDestroyShaderModule(device_, vs, nullptr);
            std::fprintf(stderr, "[postprocess] pass '%s': shader load failed\n",
                         cp.name.c_str());
            all_ok = false;
            continue;
        }

        GraphicsPipelineDesc gd;
        gd.vert        = vs;
        gd.frag        = fs;
        gd.render_pass = cp.render_pass;
        gd.subpass     = 0;
        gd.layout      = cp.layout;
        gd.cull_back   = false;  // fullscreen triangle — カリング無し
        gd.depth_test  = false;  // post-process — 深度テスト無し
        // vertex_layout 既定 (空) — 頂点入力空 = gl_VertexIndex 駆動。

        const bool ok = build_graphics_pipeline(device_, gd, cp.pipeline);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, fs, nullptr);
        if (!ok) {
            std::fprintf(stderr, "[postprocess] pass '%s': pipeline create failed\n",
                         cp.name.c_str());
            cp.pipeline = VK_NULL_HANDLE;
            all_ok = false;
        }
    }
    return all_ok;
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

int32_t PostProcessPipeline::resolve_target_(const std::string& name) const {
    if (name == kPostProcessOutputTarget) return -1;  // swapchain
    auto it = target_index_.find(name);
    if (it != target_index_.end()) return it->second;
    return -2;  // 不明 (LUT は呼び出し側が個別解決)
}

// chain の `__history:<source>__` 入力を集め、 persistent image を確保して
// 黒クリア + SHADER_READ_ONLY へ遷移させる (初回フレームの read を有効化)。
// source ターゲットの存在は build 側 (input 解決) で検証済み。
bool PostProcessPipeline::create_history_textures_() {
    history_.clear();

    // 参照される history source 名を出現順・重複なしで収集する。
    std::vector<std::string> sources;
    for (const auto& def : chain_.passes) {
        for (const auto& in_name : def.inputs) {
            const std::string src = parse_history_input(in_name);
            if (src.empty()) continue;
            bool have = false;
            for (const auto& s : sources)
                if (s == src) { have = true; break; }
            if (!have) sources.push_back(src);
        }
    }
    if (sources.empty()) return true;

    for (const auto& src : sources) {
        HistoryEntry entry;
        entry.source       = src;
        entry.source_index = resolve_target_(src);
        if (entry.source_index < 0) {
            std::fprintf(stderr,
                         "[postprocess] history source '%s' is not a target\n",
                         src.c_str());
            return false;
        }

        // history image は source ターゲットと同解像度 (縮小ターゲット対応)。
        const VkExtent2D src_extent =
            targets_[static_cast<size_t>(entry.source_index)].extent;

        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType     = VK_IMAGE_TYPE_2D;
        ic.format        = kHdrFormat;
        ic.extent        = {src_extent.width, src_extent.height, 1};
        ic.mipLevels     = 1;
        ic.arrayLayers   = 1;
        ic.samples       = VK_SAMPLE_COUNT_1_BIT;
        ic.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ic.usage         = VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ic.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device_, &ic, nullptr, &entry.tex.image) != VK_SUCCESS)
            return false;

        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(device_, entry.tex.image, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize  = mr.size;
        ai.memoryTypeIndex = find_memory_type_(mr.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device_, &ai, nullptr, &entry.tex.memory) != VK_SUCCESS)
            return false;
        vkBindImageMemory(device_, entry.tex.image, entry.tex.memory, 0);

        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image    = entry.tex.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format   = kHdrFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &vi, nullptr, &entry.tex.view) != VK_SUCCESS)
            return false;

        // 黒クリア + SHADER_READ_ONLY へ遷移 (初回フレームから read 可能に)。
        VkCommandBuffer cmd = vk_->begin_single_time_commands();
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = entry.tex.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &b);
        VkClearColorValue black{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, entry.tex.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &black, 1, &range);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &b);
        vk_->end_single_time_commands(cmd);

        history_.push_back(entry);
    }
    return true;
}

// フレーム末尾の history 更新: source ターゲット → history image をコピー。
// source は直前の render pass の finalLayout (SHADER_READ_ONLY)、 history は
// 今フレームの read 完了後 (FRAGMENT stage) に TRANSFER_DST へ遷移させる。
void PostProcessPipeline::record_history_copies_(VkCommandBuffer cmd) {
    for (const auto& entry : history_) {
        if (entry.source_index < 0) continue;
        const auto& src_rt = targets_[static_cast<size_t>(entry.source_index)];
        VkImage src = src_rt.image;

        VkImageMemoryBarrier pre[2]{};
        // source: SHADER_READ_ONLY → TRANSFER_SRC
        pre[0] = VkImageMemoryBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        pre[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        pre[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        pre[0].oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        pre[0].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        pre[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pre[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pre[0].image = src;
        pre[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        // history: SHADER_READ_ONLY → TRANSFER_DST (今フレームの read 完了後)
        pre[1] = pre[0];
        pre[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        pre[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        pre[1].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        pre[1].image         = entry.tex.image;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 2, pre);

        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent         = {src_rt.extent.width, src_rt.extent.height, 1};
        vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       entry.tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &region);

        VkImageMemoryBarrier post[2]{};
        // source: TRANSFER_SRC → SHADER_READ_ONLY (次フレームの入力へ戻す)
        post[0] = pre[0];
        post[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        post[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        post[0].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        post[0].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        // history: TRANSFER_DST → SHADER_READ_ONLY (次フレームの read へ)
        post[1] = post[0];
        post[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        post[1].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        post[1].image         = entry.tex.image;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 2, post);
    }
}

// chain_ から CompiledPass 列 + render pass / target / descriptor / pipeline を
// 構築する。 初期化 / resize の共通ボディ。
bool PostProcessPipeline::build_from_chain_(
        VkFormat output_format,
        const std::vector<VkImageView>& output_views) {
    // ── render passes ──
    if (rp_scene_ == VK_NULL_HANDLE && !create_scene_render_pass_()) {
        std::fprintf(stderr, "[postprocess] scene render pass creation failed\n");
        return false;
    }
    VkRenderPass rp_inter = get_or_create_inter_render_pass_();
    VkRenderPass rp_out   = get_or_create_output_render_pass_(output_format);
    if (rp_inter == VK_NULL_HANDLE || rp_out == VK_NULL_HANDLE) {
        std::fprintf(stderr, "[postprocess] render pass creation failed\n");
        return false;
    }

    // ── targets ──
    if (!create_targets_()) {
        std::fprintf(stderr, "[postprocess] target creation failed\n");
        return false;
    }

    // ── history buffers (`__history:*__` 入力の解決先) ──
    if (!create_history_textures_()) {
        std::fprintf(stderr, "[postprocess] history buffer creation failed\n");
        return false;
    }

    // ── pass を CompiledPass へ解決する ──
    //    LUT 入力は論理名 "__lut__" を持つ — input index = -1 で表現する。
    compiled_.clear();
    compiled_.reserve(chain_.passes.size());
    for (const auto& def : chain_.passes) {
        CompiledPass cp;
        cp.name        = def.name;
        cp.push_data   = def.push_data;
        cp.push_size   = def.push_size();
        cp.input_count = static_cast<uint32_t>(def.inputs.size());
        cp.viewport_w  = def.viewport_w;
        cp.viewport_h  = def.viewport_h;

        // 入力ターゲットを index へ解決する。
        cp.input_indices.reserve(def.inputs.size());
        for (const auto& in_name : def.inputs) {
            if (in_name == kPostProcessLutTarget) {
                cp.input_indices.push_back(-1);  // LUT (専用 Texture)
                continue;
            }
            if (in_name == kPostProcessDepthTarget) {
                cp.input_indices.push_back(-2);  // scene 深度ビュー
                continue;
            }
            const std::string hist_src = parse_history_input(in_name);
            if (!hist_src.empty()) {
                int32_t hidx = -1;
                for (size_t h = 0; h < history_.size(); ++h) {
                    if (history_[h].source == hist_src) {
                        hidx = static_cast<int32_t>(h);
                        break;
                    }
                }
                if (hidx < 0) {
                    std::fprintf(stderr,
                                 "[postprocess] pass '%s': unresolved history '%s'\n",
                                 def.name.c_str(), in_name.c_str());
                    return false;
                }
                cp.input_indices.push_back(-3 - hidx);  // history_[hidx]
                continue;
            }
            int32_t idx = resolve_target_(in_name);
            if (idx < 0) {
                std::fprintf(stderr,
                             "[postprocess] pass '%s': unknown input target '%s'\n",
                             def.name.c_str(), in_name.c_str());
                return false;
            }
            cp.input_indices.push_back(idx);
        }

        // 出力ターゲット + 対応する render pass を解決する。
        if (def.output == kPostProcessOutputTarget) {
            cp.output_index = -1;            // swapchain
            cp.render_pass  = rp_out;
        } else {
            int32_t idx = resolve_target_(def.output);
            if (idx < 0) {
                std::fprintf(stderr,
                             "[postprocess] pass '%s': unknown output target '%s'\n",
                             def.name.c_str(), def.output.c_str());
                return false;
            }
            cp.output_index = idx;
            cp.render_pass  = targets_[static_cast<size_t>(idx)].fb_render_pass;
        }

        cp.layout = get_or_create_layout_(cp.input_count, cp.push_size);
        if (cp.layout == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[postprocess] pass '%s': pipeline layout failed\n",
                         def.name.c_str());
            return false;
        }
        compiled_.push_back(std::move(cp));
    }

    // ── descriptor sets + pipelines ──
    if (!create_descriptors_()) {
        std::fprintf(stderr, "[postprocess] descriptor setup failed\n");
        return false;
    }
    if (!create_pipelines_()) {
        std::fprintf(stderr, "[postprocess] pipeline creation failed\n");
        return false;
    }
    if (!create_output_framebuffers_(output_views)) {
        std::fprintf(stderr, "[postprocess] output framebuffer creation failed\n");
        return false;
    }
    return true;
}

bool PostProcessPipeline::initialize_chain(
        VulkanContext& vk,
        const std::string& shader_dir,
        uint32_t width, uint32_t height,
        VkFormat output_format,
        const std::vector<VkImageView>& output_views,
        const PostProcessConfig& config,
        const PostProcessChain& chain,
        const unsigned char* lut_rgba,
        int lut_w, int lut_h) {
    vk_      = &vk;
    device_  = vk.device();
    width_   = width;
    height_  = height;
    extent_  = {width, height};
    config_  = config;
    chain_   = chain;
    shader_dir_     = shader_dir;
    output_format_  = output_format;
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

    if (!create_samplers_()) {
        std::fprintf(stderr, "[postprocess] sampler creation failed\n");
        return false;
    }
    if (!upload_lut_(lut_rgba, lut_w, lut_h)) {
        std::fprintf(stderr, "[postprocess] LUT setup failed\n");
        return false;
    }
    if (!build_from_chain_(output_format, output_views)) return false;

    vulkan_ready_ = true;
    std::fprintf(stderr, "[postprocess] ready: %ux%u, %zu pass, lut=%s\n",
                 width, height, compiled_.size(),
                 lut_loaded_ ? config_.color_grading.lut_path.c_str() : "(none)");
    return true;
}

bool PostProcessPipeline::initialize_vulkan(
        VulkanContext& vk,
        const std::string& shader_dir,
        uint32_t width, uint32_t height,
        VkFormat output_format,
        const std::vector<VkImageView>& output_views,
        const PostProcessConfig& config,
        const unsigned char* lut_rgba,
        int lut_w, int lut_h) {
    // 組み込みチェーン (extract → blur H → blur V → grade) を生成して
    // initialize_chain() に委譲する。 lut_loaded はこの時点で未確定なので、
    // lut_rgba の有無で先読みし、 upload 後に refresh で正しい値へ詰め直す。
    const bool lut_expected = (lut_rgba != nullptr && lut_w > 0 && lut_h > 0);
    PostProcessChain chain = build_post_process_chain(
        config, shader_dir, width, height, is_srgb_format(output_format),
        lut_expected, /*extra=*/{});
    return initialize_chain(vk, shader_dir, width, height, output_format,
                            output_views, config, chain, lut_rgba, lut_w, lut_h);
}

void PostProcessPipeline::record(VkCommandBuffer cmd, uint32_t output_index,
                                 float delta_time) {
    if (!vulkan_ready_ || output_index >= output_fbs_.size()) return;

    // auto exposure の時間適応に使う経過秒を config へ詰める (ランタイム
    // フィールド — refresh の make_exposure_measure_pc が読む)。
    if (delta_time > 0.0f) config_.hdr.delta_seconds = delta_time;

    // 組み込み pass の push_data を現フレームの config / 解像度へ詰め直す。
    // (PostProcessTuner のライブ編集を毎フレーム反映するため。 旧 record()
    //  が push constant をその場で組んでいたのと等価。)
    refresh_post_process_chain(chain_, config_, extent_.width, extent_.height,
                               output_is_srgb_, lut_loaded_);
    for (auto& cp : compiled_) {
        for (const auto& p : chain_.passes)
            if (p.name == cp.name) { cp.push_data = p.push_data; break; }
    }

    // chain の pass を挿入順に記録する。 各 pass は自前の render pass を
    // begin/end する — render pass の EXTERNAL↔0 subpass dependency が
    // pass 間の read-after-write / write-after-read を保証する。
    for (const auto& cp : compiled_) {
        if (cp.pipeline == VK_NULL_HANDLE) continue;

        VkFramebuffer fb = VK_NULL_HANDLE;
        if (cp.output_index < 0) {
            fb = output_fbs_[output_index];
        } else {
            fb = targets_[static_cast<size_t>(cp.output_index)].fb;
        }

        // 描画領域: 出力ターゲットの実解像度 (縮小ターゲットは小さい)。
        // viewport 上書き (exposure_measure の 1x1 等) があればそれが勝つ。
        VkExtent2D pass_extent =
            (cp.output_index >= 0)
                ? targets_[static_cast<size_t>(cp.output_index)].extent
                : extent_;
        if (cp.viewport_w > 0 && cp.viewport_h > 0) {
            pass_extent = {std::min(cp.viewport_w, pass_extent.width),
                           std::min(cp.viewport_h, pass_extent.height)};
        }
        VkViewport vp{0.0f, 0.0f, static_cast<float>(pass_extent.width),
                      static_cast<float>(pass_extent.height), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, pass_extent};

        VkRenderPassBeginInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpi.renderPass      = cp.render_pass;
        rpi.framebuffer     = fb;
        rpi.renderArea      = {{0, 0}, pass_extent};
        rpi.clearValueCount = 0;
        vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cp.pipeline);
        if (cp.desc_set != VK_NULL_HANDLE)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    cp.layout, 0, 1, &cp.desc_set, 0, nullptr);
        if (cp.push_size > 0 && !cp.push_data.empty())
            vkCmdPushConstants(cmd, cp.layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, cp.push_size, cp.push_data.data());
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    // history buffer 更新 (source ターゲット → persistent image のコピー)。
    record_history_copies_(cmd);
}

bool PostProcessPipeline::resize(uint32_t width, uint32_t height,
                                 const std::vector<VkImageView>& output_views) {
    if (!vulkan_ready_) return false;
    if (width == extent_.width && height == extent_.height &&
        output_views.size() == output_fbs_.size())
        return true;  // 変化なし

    vkDeviceWaitIdle(device_);

    // サイズ依存リソースのみ破棄 (render pass / sampler / LUT / descriptor
    // set layout / pipeline layout は維持)。
    destroy_chain_resources_();

    width_  = width;
    height_ = height;
    extent_ = {width, height};

    // build_from_chain_ は render pass を再利用 (rp_* が非 null なら維持)。
    if (!build_from_chain_(output_format_, output_views)) {
        std::fprintf(stderr, "[postprocess] resize: rebuild failed\n");
        vulkan_ready_ = false;
        return false;
    }
    return true;
}

bool PostProcessPipeline::rebuild_chain(
        const PostProcessChain& chain,
        const std::vector<VkImageView>& output_views) {
    if (!vulkan_ready_) return false;

    vkDeviceWaitIdle(device_);

    // resize と同じチェーン依存リソースを破棄し、 新チェーン記述で組み直す。
    // pass 数 / 入出力配線 / シェーダが変わるため pipeline / descriptor も
    // 全て作り直しになる (render pass / sampler / LUT / layout キャッシュは
    // input 数 / push サイズ単位の共有物なので維持できる)。
    destroy_chain_resources_();
    chain_ = chain;

    if (!build_from_chain_(output_format_, output_views)) {
        std::fprintf(stderr, "[postprocess] rebuild_chain: rebuild failed\n");
        vulkan_ready_ = false;
        return false;
    }
    std::fprintf(stderr, "[postprocess] chain rebuilt: %zu pass\n",
                 compiled_.size());
    return true;
}

// targets / output framebuffers / pipelines / descriptor pool を破棄する。
// 呼出し側で GPU idle を保証すること (resize / rebuild_chain 共通ボディ)。
void PostProcessPipeline::destroy_chain_resources_() {
    auto destroy_rt = [&](RenderTarget& rt) {
        if (rt.fb)           vkDestroyFramebuffer(device_, rt.fb, nullptr);
        if (rt.view)         vkDestroyImageView(device_, rt.view, nullptr);
        if (rt.image)        vkDestroyImage(device_, rt.image, nullptr);
        if (rt.memory)       vkFreeMemory(device_, rt.memory, nullptr);
        if (rt.depth_view)   vkDestroyImageView(device_, rt.depth_view, nullptr);
        if (rt.depth_image)  vkDestroyImage(device_, rt.depth_image, nullptr);
        if (rt.depth_memory) vkFreeMemory(device_, rt.depth_memory, nullptr);
    };
    for (VkFramebuffer fb : output_fbs_)
        if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
    output_fbs_.clear();
    for (auto& rt : targets_) destroy_rt(rt);
    targets_.clear();
    target_index_.clear();
    scene_index_ = -1;
    fb_scene_    = VK_NULL_HANDLE;

    // history image (resize / rebuild で内容は失われる — ホストは
    // history_valid を倒して再シードする契約)。
    for (auto& h : history_) {
        if (h.tex.view)   vkDestroyImageView(device_, h.tex.view, nullptr);
        if (h.tex.image)  vkDestroyImage(device_, h.tex.image, nullptr);
        if (h.tex.memory) vkFreeMemory(device_, h.tex.memory, nullptr);
    }
    history_.clear();

    for (auto& cp : compiled_)
        if (cp.pipeline) vkDestroyPipeline(device_, cp.pipeline, nullptr);
    if (desc_pool_) vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
    desc_pool_ = VK_NULL_HANDLE;
    compiled_.clear();
}

#endif // PICTOR_HAS_VULKAN

void PostProcessPipeline::shutdown() {
#ifdef PICTOR_HAS_VULKAN
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        auto destroy_rt = [&](RenderTarget& rt) {
            if (rt.fb)           vkDestroyFramebuffer(device_, rt.fb, nullptr);
            if (rt.view)         vkDestroyImageView(device_, rt.view, nullptr);
            if (rt.image)        vkDestroyImage(device_, rt.image, nullptr);
            if (rt.memory)       vkFreeMemory(device_, rt.memory, nullptr);
            if (rt.depth_view)   vkDestroyImageView(device_, rt.depth_view, nullptr);
            if (rt.depth_image)  vkDestroyImage(device_, rt.depth_image, nullptr);
            if (rt.depth_memory) vkFreeMemory(device_, rt.depth_memory, nullptr);
        };
        for (VkFramebuffer fb : output_fbs_)
            if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
        output_fbs_.clear();
        for (auto& rt : targets_) destroy_rt(rt);
        targets_.clear();
        target_index_.clear();
        scene_index_ = -1;
        fb_scene_    = VK_NULL_HANDLE;

        for (auto& h : history_) {
            if (h.tex.view)   vkDestroyImageView(device_, h.tex.view, nullptr);
            if (h.tex.image)  vkDestroyImage(device_, h.tex.image, nullptr);
            if (h.tex.memory) vkFreeMemory(device_, h.tex.memory, nullptr);
        }
        history_.clear();

        if (lut_.view)   vkDestroyImageView(device_, lut_.view, nullptr);
        if (lut_.image)  vkDestroyImage(device_, lut_.image, nullptr);
        if (lut_.memory) vkFreeMemory(device_, lut_.memory, nullptr);
        lut_ = Texture{};

        for (auto& cp : compiled_)
            if (cp.pipeline) vkDestroyPipeline(device_, cp.pipeline, nullptr);
        compiled_.clear();

        for (const auto& kv : layout_cache_)
            if (kv.second) vkDestroyPipelineLayout(device_, kv.second, nullptr);
        layout_cache_.clear();

        if (desc_pool_) vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
        desc_pool_ = VK_NULL_HANDLE;

        for (const auto& kv : dsl_by_input_count_)
            if (kv.second) vkDestroyDescriptorSetLayout(device_, kv.second, nullptr);
        dsl_by_input_count_.clear();

        if (sampler_)       vkDestroySampler(device_, sampler_, nullptr);
        if (depth_sampler_) vkDestroySampler(device_, depth_sampler_, nullptr);
        if (rp_scene_)  vkDestroyRenderPass(device_, rp_scene_, nullptr);
        if (rp_inter_)  vkDestroyRenderPass(device_, rp_inter_, nullptr);
        if (rp_output_) vkDestroyRenderPass(device_, rp_output_, nullptr);
        sampler_  = VK_NULL_HANDLE;
        depth_sampler_ = VK_NULL_HANDLE;
        rp_scene_ = rp_inter_ = rp_output_ = VK_NULL_HANDLE;
        device_   = VK_NULL_HANDLE;
    }
    vk_ = nullptr;
#endif
    vulkan_ready_ = false;
}

} // namespace pictor
