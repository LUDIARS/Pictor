#pragma once

#include <cstdint>

namespace pictor {

/// Platform-agnostic native window handle.
///
/// Pictor is an embeddable rendering module — the host application owns
/// the window.  This struct carries whatever OS-specific handles the
/// Vulkan back-end needs to create a VkSurfaceKHR.
///
/// Usage:
///   NativeWindowHandle h{};
///   h.type = NativeWindowHandle::Type::WIN32;
///   h.win32.hwnd      = myHwnd;
///   h.win32.hinstance = myHinstance;
struct NativeWindowHandle {
    enum class Type : uint8_t {
        NONE    = 0,
        WIN32   = 1,   // VK_KHR_win32_surface
        XLIB    = 2,   // VK_KHR_xlib_surface
        XCB     = 3,   // VK_KHR_xcb_surface
        WAYLAND = 4,   // VK_KHR_wayland_surface
        COCOA   = 5,   // VK_MVK_macos_surface / VK_EXT_metal_surface
        ANDROID = 6,   // VK_KHR_android_surface
    };

    Type type = Type::NONE;

    // Win32
    struct Win32 {
        void* hwnd      = nullptr;   // HWND
        void* hinstance = nullptr;   // HINSTANCE
    };

    // Xlib
    struct Xlib {
        void*    display = nullptr;  // Display*
        uint64_t window  = 0;        // Window (XID)
    };

    // XCB
    struct Xcb {
        void*    connection = nullptr; // xcb_connection_t*
        uint32_t window     = 0;       // xcb_window_t
    };

    // Wayland
    struct Wayland {
        void* display = nullptr; // wl_display*
        void* surface = nullptr; // wl_surface*
    };

    // macOS / Metal
    struct Cocoa {
        void* ns_view = nullptr; // NSView* (or CAMetalLayer*)
    };

    // Android
    struct Android {
        void* native_window = nullptr; // ANativeWindow*
    };

    union {
        Win32   win32;
        Xlib    xlib;
        Xcb     xcb;
        Wayland wayland;
        Cocoa   cocoa;
        Android android;
    };

    NativeWindowHandle() : win32{} {}
};

/// Swapchain configuration requested by the host.
struct SwapchainConfig {
    uint32_t width          = 0;
    uint32_t height         = 0;
    bool     vsync          = true;
    uint32_t image_count    = 3;   // triple-buffering default
};

/// ISurfaceProvider — abstraction that decouples Pictor from windowing.
///
/// Two usage patterns:
///
/// 1. **External window (embedded)** — The host creates the window,
///    implements ISurfaceProvider, and passes it to PictorRenderer.
///    Pictor only calls get_native_handle() / get_swapchain_config()
///    and creates a Vulkan surface internally.
///
/// 2. **Built-in GLFW window (demo/standalone)** — Pictor ships a
///    GlfwSurfaceProvider that owns a GLFW window and implements
///    this interface.
class ISurfaceProvider {
public:
    virtual ~ISurfaceProvider() = default;

    /// Return the platform-specific handle Pictor needs for VkSurfaceKHR.
    virtual NativeWindowHandle get_native_handle() const = 0;

    /// Return desired swapchain dimensions + settings.
    virtual SwapchainConfig get_swapchain_config() const = 0;

    /// Notify the provider that the swapchain was (re-)created with
    /// the given actual dimensions (may differ from requested ones).
    virtual void on_swapchain_created(uint32_t width, uint32_t height) {
        (void)width; (void)height;
    }

    /// Called once per frame — lets a GLFW provider poll events.
    virtual void poll_events() {}

    /// Should the application keep running?  (e.g. !glfwWindowShouldClose)
    virtual bool should_close() const { return false; }

    /// Query the current required Vulkan instance extensions
    /// (e.g. VK_KHR_surface + platform extension).
    /// Returns count; fills out_names up to max_count.
    virtual uint32_t get_required_instance_extensions(
        const char** out_names, uint32_t max_count) const {
        (void)out_names; (void)max_count;
        return 0;
    }
};

} // namespace pictor
