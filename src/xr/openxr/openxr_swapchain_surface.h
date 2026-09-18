#pragma once

#include "openxr_common.h"
#include "pictor/xr/stereo_output_surface.h"

#include <vector>

namespace pictor::xr::detail {

/// OpenXR の swapchain (2 層の画像配列) を IStereoOutputSurface として見せる。
/// XrSwapchain と、 その画像に対する VkImageView を所有する。
class OpenXrSwapchainSurface final : public IStereoOutputSurface {
public:
    OpenXrSwapchainSurface() = default;
    ~OpenXrSwapchainSurface() override;

    OpenXrSwapchainSurface(const OpenXrSwapchainSurface&)            = delete;
    OpenXrSwapchainSurface& operator=(const OpenXrSwapchainSurface&) = delete;

    bool initialize(XrSession session, VkDevice device, EyeExtent extent);
    void shutdown();

    XrSwapchain handle() const { return swapchain_; }

    EyeExtent eye_extent() const override { return extent_; }
    uint32_t  image_count() const override { return static_cast<uint32_t>(images_.size()); }
    bool      acquire(uint32_t& out_image_index) override;
    bool      release() override;

    VkFormat    color_format() const override { return format_; }
    VkImageView eye_view(uint32_t image_index, Eye eye) const override;
    VkImageView array_view(uint32_t image_index) const override;

private:
    bool choose_format_(XrSession session);
    bool create_views_();

    XrSwapchain swapchain_   = XR_NULL_HANDLE;
    VkDevice    device_      = VK_NULL_HANDLE;
    VkFormat    format_      = VK_FORMAT_UNDEFINED;
    EyeExtent   extent_;
    bool        is_acquired_ = false;

    // image index で引く平坦な配列。 eye_views_ は [image * 2 + eye]。
    std::vector<VkImage>     images_;
    std::vector<VkImageView> eye_views_;
    std::vector<VkImageView> array_views_;
};

} // namespace pictor::xr::detail
