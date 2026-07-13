#include "pictor/gi/gi_shadow_atlas.h"

#ifdef PICTOR_HAS_VULKAN
#include "pictor/surface/vulkan_context.h"
#endif

#include <algorithm>
#include <cstdio>

namespace pictor {

GIShadowAtlas::~GIShadowAtlas() {
    shutdown();
}

#ifdef PICTOR_HAS_VULKAN

namespace {

constexpr VkFormat kShadowFormat = VK_FORMAT_D32_SFLOAT;

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

} // namespace

bool GIShadowAtlas::initialize(VulkanContext& vk, uint32_t resolution,
                               uint32_t cascade_count) {
    vk_            = &vk;
    device_        = vk.device();
    resolution_    = std::max(resolution, 64u);
    cascade_count_ = std::clamp(cascade_count, 1u, kMaxCascades);

    // ── depth array image ──
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType     = VK_IMAGE_TYPE_2D;
    ic.format        = kShadowFormat;
    ic.extent        = {resolution_, resolution_, 1};
    ic.mipLevels     = 1;
    ic.arrayLayers   = cascade_count_;
    ic.samples       = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ic.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT;
    ic.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ic, nullptr, &atlas_image_) != VK_SUCCESS) {
        std::fprintf(stderr, "[gi-shadow] atlas image creation failed\n");
        return false;
    }

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(device_, atlas_image_, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = find_memory_type(vk.physical_device(),
                                          mr.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &ai, nullptr, &atlas_memory_) != VK_SUCCESS) {
        shutdown();
        return false;
    }
    vkBindImageMemory(device_, atlas_image_, atlas_memory_, 0);

    // ── views (全層 array + per-cascade layer) ──
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = atlas_image_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format   = kShadowFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, cascade_count_};
    if (vkCreateImageView(device_, &vi, nullptr, &atlas_view_) != VK_SUCCESS) {
        shutdown();
        return false;
    }
    for (uint32_t c = 0; c < cascade_count_; ++c) {
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, c, 1};
        if (vkCreateImageView(device_, &vi, nullptr, &layer_views_[c]) !=
            VK_SUCCESS) {
            shutdown();
            return false;
        }
    }

    // ── render pass (depth-only、 CLEAR → SHADER_READ_ONLY) ──
    VkAttachmentDescription att{};
    att.format         = kShadowFormat;
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

    // 前フレームの shadow read → 今フレームの depth write、
    // depth write → 後段の fragment read を張る。
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments    = &att;
    rp.subpassCount    = 1;
    rp.pSubpasses      = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies   = deps;
    if (vkCreateRenderPass(device_, &rp, nullptr, &render_pass_) != VK_SUCCESS) {
        shutdown();
        return false;
    }

    // ── per-cascade framebuffers ──
    for (uint32_t c = 0; c < cascade_count_; ++c) {
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass      = render_pass_;
        fi.attachmentCount = 1;
        fi.pAttachments    = &layer_views_[c];
        fi.width           = resolution_;
        fi.height          = resolution_;
        fi.layers          = 1;
        if (vkCreateFramebuffer(device_, &fi, nullptr, &framebuffers_[c]) !=
            VK_SUCCESS) {
            shutdown();
            return false;
        }
    }

    // ── compare sampler (PCF 用)。 範囲外は白 (= 影なし) に落とす ──
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter     = VK_FILTER_LINEAR;
    si.minFilter     = VK_FILTER_LINEAR;
    si.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.borderColor   = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    si.compareEnable = VK_TRUE;
    si.compareOp     = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (vkCreateSampler(device_, &si, nullptr, &compare_sampler_) != VK_SUCCESS) {
        shutdown();
        return false;
    }

    initialized_ = true;
    std::fprintf(stderr, "[gi-shadow] atlas ready: %ux%u x%u cascades\n",
                 resolution_, resolution_, cascade_count_);
    return true;
}

#endif // PICTOR_HAS_VULKAN

void GIShadowAtlas::shutdown() {
#ifdef PICTOR_HAS_VULKAN
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (compare_sampler_) vkDestroySampler(device_, compare_sampler_, nullptr);
        for (uint32_t c = 0; c < kMaxCascades; ++c) {
            if (framebuffers_[c])
                vkDestroyFramebuffer(device_, framebuffers_[c], nullptr);
            if (layer_views_[c])
                vkDestroyImageView(device_, layer_views_[c], nullptr);
            framebuffers_[c] = VK_NULL_HANDLE;
            layer_views_[c]  = VK_NULL_HANDLE;
        }
        if (render_pass_)  vkDestroyRenderPass(device_, render_pass_, nullptr);
        if (atlas_view_)   vkDestroyImageView(device_, atlas_view_, nullptr);
        if (atlas_image_)  vkDestroyImage(device_, atlas_image_, nullptr);
        if (atlas_memory_) vkFreeMemory(device_, atlas_memory_, nullptr);
        compare_sampler_ = VK_NULL_HANDLE;
        render_pass_     = VK_NULL_HANDLE;
        atlas_view_      = VK_NULL_HANDLE;
        atlas_image_     = VK_NULL_HANDLE;
        atlas_memory_    = VK_NULL_HANDLE;
        device_          = VK_NULL_HANDLE;
    }
    vk_ = nullptr;
#endif
    initialized_ = false;
}

} // namespace pictor
