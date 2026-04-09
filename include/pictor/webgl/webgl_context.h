#pragma once

#ifdef PICTOR_HAS_WEBGL

#include <GLES3/gl3.h>
#include <emscripten/html5.h>
#include <cstdint>
#include <string>

namespace pictor {

/// Configuration for WebGL2 context creation.
struct WebGLContextConfig {
    const char* canvas_selector = "#canvas";  // CSS selector for <canvas>
    bool        antialias       = true;
    bool        alpha           = false;
    bool        depth           = true;
    bool        stencil         = false;
    bool        premultiplied_alpha = false;
    bool        preserve_drawing_buffer = false;
    int         major_version   = 2;  // WebGL2
};

/// Capability flags queried from the WebGL2 context.
struct WebGLCapabilities {
    int32_t  max_texture_size          = 0;
    int32_t  max_texture_units         = 0;
    int32_t  max_vertex_attribs        = 0;
    int32_t  max_uniform_buffer_bindings = 0;
    int32_t  max_draw_buffers          = 0;
    int32_t  max_color_attachments     = 0;
    int32_t  max_renderbuffer_size     = 0;
    int32_t  max_viewport_dims[2]      = {};
    bool     has_float_textures        = false;  // EXT_color_buffer_float
    bool     has_anisotropic_filter    = false;  // EXT_texture_filter_anisotropic
    float    max_anisotropy            = 1.0f;
    bool     has_parallel_compile      = false;  // KHR_parallel_shader_compile
    bool     has_multi_draw            = false;  // WEBGL_multi_draw
    std::string renderer;
    std::string vendor;
    std::string version;
    std::string shading_language_version;
};

/// Manages a WebGL2 rendering context bound to an HTML <canvas> element.
///
/// Ownership model:
///   - WebGLContext **owns** the EMSCRIPTEN_WEBGL_CONTEXT_HANDLE.
///   - The <canvas> element is **borrowed** (HTML page retains ownership).
///
/// Lifecycle:
///   initialize(config) -> [begin_frame -> ... -> end_frame]* -> shutdown
class WebGLContext {
public:
    WebGLContext();
    ~WebGLContext();

    WebGLContext(const WebGLContext&) = delete;
    WebGLContext& operator=(const WebGLContext&) = delete;

    /// Create and activate a WebGL2 context on the specified canvas.
    bool initialize(const WebGLContextConfig& config = {});

    /// Destroy the context.
    void shutdown();

    /// Resize the drawing buffer to match canvas CSS dimensions.
    /// Call on window resize events.
    void resize(uint32_t width, uint32_t height);

    /// Begin a new frame: set viewport, clear buffers.
    void begin_frame(float r = 0.1f, float g = 0.1f, float b = 0.1f, float a = 1.0f);

    /// End frame (no-op for WebGL; browser composites automatically).
    void end_frame();

    /// Enable/disable depth test.
    void set_depth_test(bool enabled);

    /// Enable/disable back-face culling.
    void set_cull_face(bool enabled);

    /// Set blend mode (for transparent pass).
    void set_blend(bool enabled);

    /// Query capabilities (call after initialize).
    const WebGLCapabilities& capabilities() const { return caps_; }

    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }
    bool is_initialized() const { return initialized_; }

    /// Get pixel ratio for HiDPI displays.
    float device_pixel_ratio() const { return dpr_; }

private:
    void query_capabilities();

    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE gl_context_ = 0;
    uint32_t width_  = 0;
    uint32_t height_ = 0;
    float    dpr_    = 1.0f;
    bool     initialized_ = false;
    WebGLCapabilities caps_;
};

} // namespace pictor

#endif // PICTOR_HAS_WEBGL
