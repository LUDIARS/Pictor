#include "openxr_swapchain_surface.h"

#include <algorithm>

namespace pictor::xr::detail {

namespace {

// 好ましい順。 sRGB の 8bit を優先する (HMD の合成器が sRGB として扱うため)。
constexpr VkFormat kPreferredFormats[] = {
    VK_FORMAT_R8G8B8A8_SRGB,
    VK_FORMAT_B8G8R8A8_SRGB,
    VK_FORMAT_R8G8B8A8_UNORM,
    VK_FORMAT_B8G8R8A8_UNORM,
};

VkImageView create_view(VkDevice device, VkImage image, VkFormat format,
                        VkImageViewType type, uint32_t base_layer, uint32_t layer_count) {
    VkImageViewCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image    = image;
    info.viewType = type;
    info.format   = format;
    info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    info.subresourceRange.levelCount     = 1;
    info.subresourceRange.baseArrayLayer = base_layer;
    info.subresourceRange.layerCount     = layer_count;
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &info, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    return view;
}

} // namespace

OpenXrSwapchainSurface::~OpenXrSwapchainSurface() { shutdown(); }

bool OpenXrSwapchainSurface::choose_format_(XrSession session) {
    uint32_t count = 0;
    if (!xr_check(xrEnumerateSwapchainFormats(session, 0, &count, nullptr),
                  "xrEnumerateSwapchainFormats(count)")) return false;
    std::vector<int64_t> formats(count);
    if (!xr_check(xrEnumerateSwapchainFormats(session, count, &count, formats.data()),
                  "xrEnumerateSwapchainFormats")) return false;

    for (VkFormat wanted : kPreferredFormats) {
        if (std::find(formats.begin(), formats.end(), static_cast<int64_t>(wanted)) !=
            formats.end()) {
            format_ = wanted;
            return true;
        }
    }
    std::fprintf(stderr, "[Pictor][xr] runtime offers no supported 8-bit color format\n");
    return false;
}

bool OpenXrSwapchainSurface::initialize(XrSession session, VkDevice device, EyeExtent extent) {
    if (swapchain_ != XR_NULL_HANDLE) return false;
    if (extent.width == 0 || extent.height == 0) return false;
    device_ = device;
    extent_ = extent;

    if (!choose_format_(session)) return false;

    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format      = static_cast<int64_t>(format_);
    info.sampleCount = 1;
    info.width       = extent.width;
    info.height      = extent.height;
    info.faceCount   = 1;
    info.arraySize   = kEyeCount;
    info.mipCount    = 1;
    if (!xr_check(xrCreateSwapchain(session, &info, &swapchain_), "xrCreateSwapchain")) {
        swapchain_ = XR_NULL_HANDLE;
        return false;
    }

    uint32_t count = 0;
    if (!xr_check(xrEnumerateSwapchainImages(swapchain_, 0, &count, nullptr),
                  "xrEnumerateSwapchainImages(count)")) {
        shutdown();
        return false;
    }
    XrSwapchainImageVulkanKHR blank{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
    std::vector<XrSwapchainImageVulkanKHR> xr_images(count, blank);
    if (!xr_check(xrEnumerateSwapchainImages(
                      swapchain_, count, &count,
                      reinterpret_cast<XrSwapchainImageBaseHeader*>(xr_images.data())),
                  "xrEnumerateSwapchainImages")) {
        shutdown();
        return false;
    }
    images_.resize(count);
    for (uint32_t i = 0; i < count; ++i) images_[i] = xr_images[i].image;

    if (!create_views_()) {
        shutdown();
        return false;
    }
    return true;
}

bool OpenXrSwapchainSurface::create_views_() {
    eye_views_.assign(images_.size() * kEyeCount, VK_NULL_HANDLE);
    array_views_.assign(images_.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < images_.size(); ++i) {
        for (uint32_t eye = 0; eye < kEyeCount; ++eye) {
            eye_views_[i * kEyeCount + eye] =
                create_view(device_, images_[i], format_, VK_IMAGE_VIEW_TYPE_2D, eye, 1);
            if (!eye_views_[i * kEyeCount + eye]) return false;
        }
        array_views_[i] = create_view(device_, images_[i], format_,
                                      VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, kEyeCount);
        if (!array_views_[i]) return false;
    }
    return true;
}

void OpenXrSwapchainSurface::shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        for (VkImageView v : eye_views_)   if (v) vkDestroyImageView(device_, v, nullptr);
        for (VkImageView v : array_views_) if (v) vkDestroyImageView(device_, v, nullptr);
    }
    eye_views_.clear();
    array_views_.clear();
    images_.clear();  // 画像本体は swapchain の所有
    if (swapchain_ != XR_NULL_HANDLE) {
        xrDestroySwapchain(swapchain_);
        swapchain_ = XR_NULL_HANDLE;
    }
    is_acquired_ = false;
    device_ = VK_NULL_HANDLE;
}

bool OpenXrSwapchainSurface::acquire(uint32_t& out_image_index) {
    if (swapchain_ == XR_NULL_HANDLE || is_acquired_) return false;

    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t index = 0;
    if (!xr_check(xrAcquireSwapchainImage(swapchain_, &acquire_info, &index),
                  "xrAcquireSwapchainImage")) return false;
    // acquire した画像は、 待機に失敗しても release しないと次の acquire ができない。
    is_acquired_ = true;

    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = XR_INFINITE_DURATION;
    if (!xr_check(xrWaitSwapchainImage(swapchain_, &wait_info), "xrWaitSwapchainImage")) {
        release();
        return false;
    }
    out_image_index = index;
    return true;
}

bool OpenXrSwapchainSurface::release() {
    if (swapchain_ == XR_NULL_HANDLE || !is_acquired_) return false;
    is_acquired_ = false;
    XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    return xr_check(xrReleaseSwapchainImage(swapchain_, &info), "xrReleaseSwapchainImage");
}

VkImageView OpenXrSwapchainSurface::eye_view(uint32_t image_index, Eye eye) const {
    const size_t at = static_cast<size_t>(image_index) * kEyeCount + static_cast<uint32_t>(eye);
    return at < eye_views_.size() ? eye_views_[at] : VK_NULL_HANDLE;
}

VkImageView OpenXrSwapchainSurface::array_view(uint32_t image_index) const {
    return image_index < array_views_.size() ? array_views_[image_index] : VK_NULL_HANDLE;
}

} // namespace pictor::xr::detail
