#pragma once

#include "vr_vulkan_util.h"
#include "pictor/xr/stereo_output_surface.h"

#include <vector>

namespace vr_demo {

/// HMD の描画先まわりの Vulkan 資源 (render pass / framebuffer / 深度 / キーフレーム)。
///
/// render pass は 3 種類あり、 どれも「色 1 + 深度 1、 同じ format」 なので
/// 眼ごとの 2 つは互換 (同じ pipeline で描ける)。 multiview だけは別 pipeline が要る。
///   eye_pass       … HMD の画像の片眼ぶんへ描く
///   keyframe_pass  … 静的層をキーフレームへ描き、 後で再投影元として読めるようにする
///   multiview_pass … HMD の画像の 2 層へ 1 パスで描く
class VrEyeTargets {
public:
    VrEyeTargets() = default;
    ~VrEyeTargets();

    VrEyeTargets(const VrEyeTargets&)            = delete;
    VrEyeTargets& operator=(const VrEyeTargets&) = delete;

    bool initialize(VkPhysicalDevice physical, VkDevice device,
                    pictor::xr::IStereoOutputSurface& surface, bool with_multiview);
    void shutdown();

    VkExtent2D   extent() const { return extent_; }
    VkRenderPass eye_pass() const { return eye_pass_; }
    VkRenderPass keyframe_pass() const { return keyframe_pass_; }
    VkRenderPass multiview_pass() const { return multiview_pass_; }

    VkFramebuffer eye_framebuffer(uint32_t image_index, uint32_t eye) const {
        return eye_framebuffers_[image_index * 2 + eye];
    }
    VkFramebuffer keyframe_framebuffer(uint32_t eye) const { return keyframe_framebuffers_[eye]; }
    VkFramebuffer multiview_framebuffer(uint32_t image_index) const {
        return multiview_framebuffers_[image_index];
    }

    VkImageView keyframe_color(uint32_t eye) const { return keyframe_color_[eye].view; }
    VkImageView keyframe_depth(uint32_t eye) const { return keyframe_depth_[eye].view; }

private:
    bool create_passes_(VkFormat color_format, bool with_multiview);
    bool create_images_(VkPhysicalDevice physical, VkFormat color_format, bool with_multiview);
    bool create_framebuffers_(pictor::xr::IStereoOutputSurface& surface, bool with_multiview);

    VkDevice     device_         = VK_NULL_HANDLE;
    VkExtent2D   extent_         = {0, 0};
    VkRenderPass eye_pass_       = VK_NULL_HANDLE;
    VkRenderPass keyframe_pass_  = VK_NULL_HANDLE;
    VkRenderPass multiview_pass_ = VK_NULL_HANDLE;

    Image eye_depth_[2];
    Image multiview_depth_;
    Image keyframe_color_[2];
    Image keyframe_depth_[2];

    std::vector<VkFramebuffer> eye_framebuffers_;        // [image * 2 + eye]
    std::vector<VkFramebuffer> multiview_framebuffers_;  // [image]
    VkFramebuffer              keyframe_framebuffers_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
};

} // namespace vr_demo
