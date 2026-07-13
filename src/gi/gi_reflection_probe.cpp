#include "pictor/gi/gi_reflection_probe.h"

#ifdef PICTOR_HAS_VULKAN
#include "pictor/surface/vulkan_context.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pictor {

GIReflectionProbe::~GIReflectionProbe() {
    shutdown();
}

#ifdef PICTOR_HAS_VULKAN

namespace {

constexpr VkFormat kProbeFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

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

bool GIReflectionProbe::create_cubemap_(uint32_t resolution, uint32_t mips,
                                        bool renderable) {
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ic.imageType     = VK_IMAGE_TYPE_2D;
    ic.format        = kProbeFormat;
    ic.extent        = {resolution, resolution, 1};
    ic.mipLevels     = mips;
    ic.arrayLayers   = 6;
    ic.samples       = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ic.usage         = VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (renderable) ic.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ic.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ic, nullptr, &cube_image_) != VK_SUCCESS)
        return false;

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(device_, cube_image_, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = find_memory_type(vk_->physical_device(),
                                          mr.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &ai, nullptr, &cube_memory_) != VK_SUCCESS)
        return false;
    vkBindImageMemory(device_, cube_image_, cube_memory_, 0);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = cube_image_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    vi.format   = kProbeFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 6};
    return vkCreateImageView(device_, &vi, nullptr, &cube_view_) == VK_SUCCESS;
}

bool GIReflectionProbe::initialize(VulkanContext& vk, uint32_t resolution) {
    vk_         = &vk;
    device_     = vk.device();
    resolution_ = std::max(resolution, 16u);
    mip_count_  = 1 + static_cast<uint32_t>(
        std::floor(std::log2(static_cast<float>(resolution_))));

    if (!create_cubemap_(resolution_, mip_count_, /*renderable=*/true)) {
        std::fprintf(stderr, "[gi-refl] cubemap creation failed\n");
        shutdown();
        return false;
    }

    // face 描画用 render pass (mip 0 の各面へ CLEAR → TRANSFER_SRC —
    // 直後の mip blit チェーンのソースになる)。
    VkAttachmentDescription att{};
    att.format         = kProbeFormat;
    att.samples        = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout    = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

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
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

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

    for (uint32_t f = 0; f < 6; ++f) {
        VkImageViewCreateInfo fv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        fv.image    = cube_image_;
        fv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        fv.format   = kProbeFormat;
        fv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, f, 1};
        if (vkCreateImageView(device_, &fv, nullptr, &face_views_[f]) !=
            VK_SUCCESS) {
            shutdown();
            return false;
        }
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass      = render_pass_;
        fi.attachmentCount = 1;
        fi.pAttachments    = &face_views_[f];
        fi.width           = resolution_;
        fi.height          = resolution_;
        fi.layers          = 1;
        if (vkCreateFramebuffer(device_, &fi, nullptr, &framebuffers_[f]) !=
            VK_SUCCESS) {
            shutdown();
            return false;
        }
    }

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod       = static_cast<float>(mip_count_);
    if (vkCreateSampler(device_, &si, nullptr, &sampler_) != VK_SUCCESS) {
        shutdown();
        return false;
    }

    initialized_ = true;
    std::fprintf(stderr, "[gi-refl] probe ready: %ux%u x6, %u mips\n",
                 resolution_, resolution_, mip_count_);
    return true;
}

bool GIReflectionProbe::initialize_fallback(VulkanContext& vk) {
    vk_         = &vk;
    device_     = vk.device();
    resolution_ = 1;
    mip_count_  = 1;

    if (!create_cubemap_(1, 1, /*renderable=*/false)) {
        shutdown();
        return false;
    }

    // 黒クリア + SHADER_READ_ONLY へ (binding を常に有効に保つ)。
    VkCommandBuffer cmd = vk.begin_single_time_commands();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = cube_image_;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &b);
    VkClearColorValue black{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
    vkCmdClearColorImage(cmd, cube_image_,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &b);
    vk.end_single_time_commands(cmd);

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &si, nullptr, &sampler_) != VK_SUCCESS) {
        shutdown();
        return false;
    }

    initialized_ = true;
    return true;
}

void GIReflectionProbe::record_mip_generation(VkCommandBuffer cmd) {
    if (!initialized_ || mip_count_ <= 1) return;

    // mip 0 は render pass の finalLayout (TRANSFER_SRC)。 mip 1.. を
    // blit で順に埋める: dst を TRANSFER_DST へ → blit → dst を
    // TRANSFER_SRC へ (次レベルのソース)。 最後に全 mip を SHADER_READ へ。
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = cube_image_;

    int32_t src_dim = static_cast<int32_t>(resolution_);
    for (uint32_t mip = 1; mip < mip_count_; ++mip) {
        const int32_t dst_dim = std::max(src_dim / 2, 1);

        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 6};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &b);

        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 6};
        blit.srcOffsets[1]  = {src_dim, src_dim, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 6};
        blit.dstOffsets[1]  = {dst_dim, dst_dim, 1};
        vkCmdBlitImage(cmd, cube_image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       cube_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);

        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &b);
        src_dim = dst_dim;
    }

    // 全 mip: TRANSFER_SRC → SHADER_READ_ONLY。
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_count_, 0, 6};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &b);
}

#endif // PICTOR_HAS_VULKAN

void GIReflectionProbe::shutdown() {
#ifdef PICTOR_HAS_VULKAN
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
        for (uint32_t f = 0; f < 6; ++f) {
            if (framebuffers_[f])
                vkDestroyFramebuffer(device_, framebuffers_[f], nullptr);
            if (face_views_[f])
                vkDestroyImageView(device_, face_views_[f], nullptr);
            framebuffers_[f] = VK_NULL_HANDLE;
            face_views_[f]   = VK_NULL_HANDLE;
        }
        if (render_pass_) vkDestroyRenderPass(device_, render_pass_, nullptr);
        if (cube_view_)   vkDestroyImageView(device_, cube_view_, nullptr);
        if (cube_image_)  vkDestroyImage(device_, cube_image_, nullptr);
        if (cube_memory_) vkFreeMemory(device_, cube_memory_, nullptr);
        sampler_     = VK_NULL_HANDLE;
        render_pass_ = VK_NULL_HANDLE;
        cube_view_   = VK_NULL_HANDLE;
        cube_image_  = VK_NULL_HANDLE;
        cube_memory_ = VK_NULL_HANDLE;
        device_      = VK_NULL_HANDLE;
    }
    vk_ = nullptr;
#endif
    initialized_ = false;
}

} // namespace pictor
