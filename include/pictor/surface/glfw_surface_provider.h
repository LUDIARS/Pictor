#pragma once

#include "pictor/surface/surface_provider.h"
#include <string>

// Forward-declare GLFW types to keep GLFW out of the public header.
struct GLFWwindow;

namespace pictor {

/// Configuration for the GLFW-based window.
struct GlfwWindowConfig {
    uint32_t    width       = 1280;
    uint32_t    height      = 720;
    std::string title       = "Pictor";
    bool        resizable   = true;
    bool        vsync       = true;
};

/// Concrete ISurfaceProvider backed by a GLFW window.
///
/// Owns the GLFW window.  Suitable for demos and standalone tools;
/// not intended for production embedding (the host would implement
/// ISurfaceProvider directly in that case).
class GlfwSurfaceProvider : public ISurfaceProvider {
public:
    GlfwSurfaceProvider();
    ~GlfwSurfaceProvider() override;

    GlfwSurfaceProvider(const GlfwSurfaceProvider&) = delete;
    GlfwSurfaceProvider& operator=(const GlfwSurfaceProvider&) = delete;

    /// Create the GLFW window (calls glfwInit on first use).
    bool create(const GlfwWindowConfig& config = {});

    /// Destroy the GLFW window.
    void destroy();

    // -- ISurfaceProvider --
    NativeWindowHandle get_native_handle() const override;
    SwapchainConfig    get_swapchain_config() const override;
    void               on_swapchain_created(uint32_t w, uint32_t h) override;
    void               poll_events() override;
    bool               should_close() const override;
    uint32_t           get_required_instance_extensions(
                           const char** out, uint32_t max) const override;

    GLFWwindow* glfw_window() const { return window_; }

private:
    static void framebuffer_size_callback(GLFWwindow* win, int w, int h);

    GLFWwindow* window_ = nullptr;
    uint32_t    fb_width_  = 0;
    uint32_t    fb_height_ = 0;
    bool        vsync_ = true;
};

} // namespace pictor
