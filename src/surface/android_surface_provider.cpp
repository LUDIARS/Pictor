#include "pictor/surface/android_surface_provider.h"

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(PICTOR_PLATFORM_ANDROID)
#include <android/native_window.h>
#endif

namespace pictor {

AndroidSurfaceProvider::AndroidSurfaceProvider(ANativeWindow* window,
                                               uint32_t width, uint32_t height)
    : window_(window), width_(width), height_(height) {}

NativeWindowHandle AndroidSurfaceProvider::get_native_handle() const {
    NativeWindowHandle h;
    if (!window_) {
        h.type = NativeWindowHandle::Type::None;
        return h;
    }
    h.type = NativeWindowHandle::Type::Android;
    h.android.native_window = window_;
    return h;
}

SwapchainConfig AndroidSurfaceProvider::get_swapchain_config() const {
    SwapchainConfig cfg;
    cfg.width       = width_;
    cfg.height      = height_;
    cfg.vsync       = true;   // モバイルは常時 vsync (発熱・電力)
    cfg.image_count = 3;
    return cfg;
}

void AndroidSurfaceProvider::on_swapchain_created(uint32_t width, uint32_t height) {
    width_  = width;
    height_ = height;
}

uint32_t AndroidSurfaceProvider::get_required_instance_extensions(
        const char** out_names, uint32_t max_count) const {
    static const char* kExts[] = {
        "VK_KHR_surface",
        "VK_KHR_android_surface",
    };
    const uint32_t n = sizeof(kExts) / sizeof(kExts[0]);
    const uint32_t count = (n < max_count) ? n : max_count;
    for (uint32_t i = 0; i < count; ++i) out_names[i] = kExts[i];
    return count;
}

void AndroidSurfaceProvider::update_window(ANativeWindow* window,
                                           uint32_t width, uint32_t height) {
    window_ = window;
    if (window) {
        width_  = width;
        height_ = height;
    }
}

} // namespace pictor
