#include "pictor/surface/ios_surface_provider.h"

namespace pictor {

IOSSurfaceProvider::IOSSurfaceProvider(void* metal_layer,
                                       uint32_t width, uint32_t height)
    : metal_layer_(metal_layer), width_(width), height_(height) {}

NativeWindowHandle IOSSurfaceProvider::get_native_handle() const {
    NativeWindowHandle h;
    if (!metal_layer_) {
        h.type = NativeWindowHandle::Type::None;
        return h;
    }
    h.type = NativeWindowHandle::Type::iOS;
    h.ios.metal_layer = metal_layer_;
    return h;
}

SwapchainConfig IOSSurfaceProvider::get_swapchain_config() const {
    SwapchainConfig cfg;
    cfg.width       = width_;
    cfg.height      = height_;
    cfg.vsync       = true;   // モバイルは常時 vsync (発熱・電力)
    cfg.image_count = 3;
    return cfg;
}

void IOSSurfaceProvider::on_swapchain_created(uint32_t width, uint32_t height) {
    width_  = width;
    height_ = height;
}

uint32_t IOSSurfaceProvider::get_required_instance_extensions(
        const char** out_names, uint32_t max_count) const {
    static const char* kExts[] = {
        "VK_KHR_surface",
        "VK_EXT_metal_surface",
    };
    const uint32_t n = sizeof(kExts) / sizeof(kExts[0]);
    const uint32_t count = (n < max_count) ? n : max_count;
    for (uint32_t i = 0; i < count; ++i) out_names[i] = kExts[i];
    return count;
}

void IOSSurfaceProvider::update_layer(void* metal_layer,
                                      uint32_t width, uint32_t height) {
    metal_layer_ = metal_layer;
    if (metal_layer) {
        width_  = width;
        height_ = height;
    }
}

} // namespace pictor
