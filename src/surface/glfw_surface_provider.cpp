#include "pictor/surface/glfw_surface_provider.h"

#ifdef PICTOR_HAS_VULKAN

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <cstdio>

// Platform-specific native access (for NativeWindowHandle)
#if defined(_WIN32)
  #define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
  // Detect Wayland vs X11 at runtime via GLFW
  #if defined(GLFW_EXPOSE_NATIVE_WAYLAND)
    // already defined
  #else
    #define GLFW_EXPOSE_NATIVE_X11
  #endif
#elif defined(__APPLE__)
  #define GLFW_EXPOSE_NATIVE_COCOA
#endif
#include <GLFW/glfw3native.h>

namespace pictor {

GlfwSurfaceProvider::GlfwSurfaceProvider() = default;

GlfwSurfaceProvider::~GlfwSurfaceProvider() {
    destroy();
}

bool GlfwSurfaceProvider::create(const GlfwWindowConfig& config) {
    if (!glfwInit()) {
        fprintf(stderr, "[Pictor/GLFW] glfwInit failed\n");
        return false;
    }

    if (!glfwVulkanSupported()) {
        fprintf(stderr, "[Pictor/GLFW] Vulkan not supported by this GLFW build\n");
        glfwTerminate();
        return false;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // No OpenGL context
    glfwWindowHint(GLFW_RESIZABLE, config.resizable ? GLFW_TRUE : GLFW_FALSE);

    window_ = glfwCreateWindow(
        static_cast<int>(config.width),
        static_cast<int>(config.height),
        config.title.c_str(),
        nullptr, nullptr);

    if (!window_) {
        fprintf(stderr, "[Pictor/GLFW] Failed to create window\n");
        glfwTerminate();
        return false;
    }

    // Store this pointer for callbacks
    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, framebuffer_size_callback);

    // Query actual framebuffer size (may differ on HiDPI)
    int w, h;
    glfwGetFramebufferSize(window_, &w, &h);
    fb_width_  = static_cast<uint32_t>(w);
    fb_height_ = static_cast<uint32_t>(h);
    vsync_     = config.vsync;

    printf("[Pictor/GLFW] Window created: %ux%u (fb %ux%u)\n",
           config.width, config.height, fb_width_, fb_height_);
    return true;
}

void GlfwSurfaceProvider::destroy() {
    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
        glfwTerminate();
    }
}

NativeWindowHandle GlfwSurfaceProvider::get_native_handle() const {
    NativeWindowHandle h{};
    if (!window_) return h;

#if defined(_WIN32)
    h.type = NativeWindowHandle::Type::Win32;
    h.win32.hwnd      = glfwGetWin32Window(window_);
    h.win32.hinstance = GetModuleHandle(nullptr);
#elif defined(__linux__)
    // X11 path (most common with GLFW on Linux)
    h.type = NativeWindowHandle::Type::Xlib;
    h.xlib.display = glfwGetX11Display();
    h.xlib.window  = glfwGetX11Window(window_);
#elif defined(__APPLE__)
    h.type = NativeWindowHandle::Type::Cocoa;
    h.cocoa.ns_view = glfwGetCocoaWindow(window_); // NSWindow* — MoltenVK accepts this
#endif

    return h;
}

SwapchainConfig GlfwSurfaceProvider::get_swapchain_config() const {
    SwapchainConfig sc{};
    sc.width       = fb_width_;
    sc.height      = fb_height_;
    sc.vsync       = vsync_;
    sc.image_count = 3;
    return sc;
}

void GlfwSurfaceProvider::on_swapchain_created(uint32_t w, uint32_t h) {
    fb_width_  = w;
    fb_height_ = h;
}

void GlfwSurfaceProvider::poll_events() {
    glfwPollEvents();
}

bool GlfwSurfaceProvider::should_close() const {
    return window_ ? glfwWindowShouldClose(window_) : true;
}

uint32_t GlfwSurfaceProvider::get_required_instance_extensions(
    const char** out, uint32_t max) const
{
    uint32_t glfw_ext_count = 0;
    const char** glfw_exts = glfwGetRequiredInstanceExtensions(&glfw_ext_count);
    uint32_t copy_count = (glfw_ext_count < max) ? glfw_ext_count : max;
    for (uint32_t i = 0; i < copy_count; ++i) {
        out[i] = glfw_exts[i];
    }
    return copy_count;
}

void GlfwSurfaceProvider::framebuffer_size_callback(GLFWwindow* win, int w, int h) {
    auto* self = static_cast<GlfwSurfaceProvider*>(glfwGetWindowUserPointer(win));
    if (self) {
        self->fb_width_  = static_cast<uint32_t>(w);
        self->fb_height_ = static_cast<uint32_t>(h);
    }
}

} // namespace pictor

#else // !PICTOR_HAS_VULKAN

#include <cstdio>

namespace pictor {

GlfwSurfaceProvider::GlfwSurfaceProvider() = default;
GlfwSurfaceProvider::~GlfwSurfaceProvider() = default;

bool GlfwSurfaceProvider::create(const GlfwWindowConfig&) {
    fprintf(stderr, "[Pictor/GLFW] Vulkan not available\n");
    return false;
}

void GlfwSurfaceProvider::destroy() {}
NativeWindowHandle GlfwSurfaceProvider::get_native_handle() const { return {}; }
SwapchainConfig    GlfwSurfaceProvider::get_swapchain_config() const { return {}; }
void               GlfwSurfaceProvider::on_swapchain_created(uint32_t, uint32_t) {}
void               GlfwSurfaceProvider::poll_events() {}
bool               GlfwSurfaceProvider::should_close() const { return true; }
uint32_t           GlfwSurfaceProvider::get_required_instance_extensions(
                       const char**, uint32_t) const { return 0; }

} // namespace pictor

#endif // PICTOR_HAS_VULKAN
