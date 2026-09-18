#include "vr_eye_targets.h"

#include <cstdio>

namespace vr_demo {

namespace {

constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

struct PassLayouts {
    VkImageLayout color_final;
    VkImageLayout depth_final;
    bool          store_depth;
    bool          is_multiview;
};

VkRenderPass create_pass(VkDevice device, VkFormat color_format, const PassLayouts& layouts) {
    VkAttachmentDescription attachments[2]{};
    attachments[0].format        = color_format;
    attachments[0].samples       = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp        = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp       = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout   = layouts.color_final;

    attachments[1].format        = kDepthFormat;
    attachments[1].samples       = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp        = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp       = layouts.store_depth ? VK_ATTACHMENT_STORE_OP_STORE
                                                       : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout   = layouts.depth_final;

    const VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    // 2 層 (左眼・右眼) を 1 パスで描く指定。 両方の層が互いに相関している。
    const uint32_t view_mask = 0b11;
    VkRenderPassMultiviewCreateInfo multiview{};
    multiview.sType                = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
    multiview.subpassCount         = 1;
    multiview.pViewMasks           = &view_mask;
    multiview.correlationMaskCount = 1;
    multiview.pCorrelationMasks    = &view_mask;

    VkRenderPassCreateInfo info{};
    info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.pNext           = layouts.is_multiview ? &multiview : nullptr;
    info.attachmentCount = 2;
    info.pAttachments    = attachments;
    info.subpassCount    = 1;
    info.pSubpasses      = &subpass;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(device, &info, nullptr, &pass) != VK_SUCCESS) return VK_NULL_HANDLE;
    return pass;
}

VkFramebuffer create_framebuffer(VkDevice device, VkRenderPass pass, VkImageView color,
                                 VkImageView depth, VkExtent2D extent) {
    const VkImageView views[2] = {color, depth};
    VkFramebufferCreateInfo info{};
    info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass      = pass;
    info.attachmentCount = 2;
    info.pAttachments    = views;
    info.width           = extent.width;
    info.height          = extent.height;
    info.layers          = 1;  // multiview でも 1 (層は view mask が決める)
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(device, &info, nullptr, &fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    return fb;
}

} // namespace

VrEyeTargets::~VrEyeTargets() { shutdown(); }

bool VrEyeTargets::initialize(VkPhysicalDevice physical, VkDevice device,
                              pictor::xr::IStereoOutputSurface& surface, bool with_multiview) {
    if (device_ != VK_NULL_HANDLE) return false;
    device_ = device;
    const pictor::xr::EyeExtent e = surface.eye_extent();
    extent_ = {e.width, e.height};

    if (!create_passes_(surface.color_format(), with_multiview) ||
        !create_images_(physical, surface.color_format(), with_multiview) ||
        !create_framebuffers_(surface, with_multiview)) {
        std::fprintf(stderr, "[vr_demo] failed to create eye targets\n");
        shutdown();
        return false;
    }
    return true;
}

bool VrEyeTargets::create_passes_(VkFormat color_format, bool with_multiview) {
    // HMD の画像は COLOR_ATTACHMENT_OPTIMAL のままランタイムへ返す決まり。
    eye_pass_ = create_pass(device_, color_format,
        {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, false, false});
    // キーフレームは描いた後に再投影元として読むので、 読み取り用の layout で終える。
    keyframe_pass_ = create_pass(device_, color_format,
        {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, true, false});
    if (!eye_pass_ || !keyframe_pass_) return false;

    if (with_multiview) {
        multiview_pass_ = create_pass(device_, color_format,
            {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
             VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, false, true});
        if (!multiview_pass_) return false;
    }
    return true;
}

bool VrEyeTargets::create_images_(VkPhysicalDevice physical, VkFormat color_format,
                                  bool with_multiview) {
    ImageDesc depth;
    depth.width  = extent_.width;
    depth.height = extent_.height;
    depth.format = kDepthFormat;
    depth.usage  = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depth.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;

    ImageDesc key_color = depth;
    key_color.format = color_format;
    key_color.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    key_color.aspect = VK_IMAGE_ASPECT_COLOR_BIT;

    ImageDesc key_depth = depth;
    key_depth.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;

    for (uint32_t eye = 0; eye < 2; ++eye) {
        if (!create_image(physical, device_, depth, eye_depth_[eye]) ||
            !create_image(physical, device_, key_color, keyframe_color_[eye]) ||
            !create_image(physical, device_, key_depth, keyframe_depth_[eye])) {
            return false;
        }
    }
    if (with_multiview) {
        ImageDesc layered = depth;
        layered.layers = 2;
        if (!create_image(physical, device_, layered, multiview_depth_)) return false;
    }
    return true;
}

bool VrEyeTargets::create_framebuffers_(pictor::xr::IStereoOutputSurface& surface,
                                        bool with_multiview) {
    using pictor::xr::Eye;
    const uint32_t image_count = surface.image_count();
    eye_framebuffers_.assign(image_count * 2, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < image_count; ++i) {
        for (uint32_t eye = 0; eye < 2; ++eye) {
            const VkFramebuffer fb = create_framebuffer(
                device_, eye_pass_, surface.eye_view(i, static_cast<Eye>(eye)),
                eye_depth_[eye].view, extent_);
            if (!fb) return false;
            eye_framebuffers_[i * 2 + eye] = fb;
        }
    }
    for (uint32_t eye = 0; eye < 2; ++eye) {
        keyframe_framebuffers_[eye] = create_framebuffer(
            device_, keyframe_pass_, keyframe_color_[eye].view, keyframe_depth_[eye].view, extent_);
        if (!keyframe_framebuffers_[eye]) return false;
    }
    if (with_multiview) {
        multiview_framebuffers_.assign(image_count, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < image_count; ++i) {
            multiview_framebuffers_[i] = create_framebuffer(
                device_, multiview_pass_, surface.array_view(i), multiview_depth_.view, extent_);
            if (!multiview_framebuffers_[i]) return false;
        }
    }
    return true;
}

void VrEyeTargets::shutdown() {
    if (device_ == VK_NULL_HANDLE) return;
    for (VkFramebuffer fb : eye_framebuffers_)       if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
    for (VkFramebuffer fb : multiview_framebuffers_) if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
    eye_framebuffers_.clear();
    multiview_framebuffers_.clear();
    for (uint32_t eye = 0; eye < 2; ++eye) {
        if (keyframe_framebuffers_[eye]) vkDestroyFramebuffer(device_, keyframe_framebuffers_[eye], nullptr);
        keyframe_framebuffers_[eye] = VK_NULL_HANDLE;
        destroy_image(device_, eye_depth_[eye]);
        destroy_image(device_, keyframe_color_[eye]);
        destroy_image(device_, keyframe_depth_[eye]);
    }
    destroy_image(device_, multiview_depth_);
    if (eye_pass_)       vkDestroyRenderPass(device_, eye_pass_, nullptr);
    if (keyframe_pass_)  vkDestroyRenderPass(device_, keyframe_pass_, nullptr);
    if (multiview_pass_) vkDestroyRenderPass(device_, multiview_pass_, nullptr);
    eye_pass_ = keyframe_pass_ = multiview_pass_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

} // namespace vr_demo
