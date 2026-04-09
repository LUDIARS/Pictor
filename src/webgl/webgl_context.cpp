#ifdef PICTOR_HAS_WEBGL

#include "pictor/webgl/webgl_context.h"
#include <emscripten.h>
#include <cstdio>

namespace pictor {

WebGLContext::WebGLContext() = default;

WebGLContext::~WebGLContext() {
    if (initialized_) shutdown();
}

bool WebGLContext::initialize(const WebGLContextConfig& config) {
    if (initialized_) return true;

    // Query device pixel ratio
    dpr_ = static_cast<float>(emscripten_get_device_pixel_ratio());

    // Set up WebGL2 attributes
    EmscriptenWebGLContextAttributes attrs;
    emscripten_webgl_init_context_attributes(&attrs);
    attrs.majorVersion             = config.major_version;
    attrs.minorVersion             = 0;
    attrs.antialias                = config.antialias ? EM_TRUE : EM_FALSE;
    attrs.alpha                    = config.alpha ? EM_TRUE : EM_FALSE;
    attrs.depth                    = config.depth ? EM_TRUE : EM_FALSE;
    attrs.stencil                  = config.stencil ? EM_TRUE : EM_FALSE;
    attrs.premultipliedAlpha       = config.premultiplied_alpha ? EM_TRUE : EM_FALSE;
    attrs.preserveDrawingBuffer    = config.preserve_drawing_buffer ? EM_TRUE : EM_FALSE;
    attrs.powerPreference          = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
    attrs.enableExtensionsByDefault = EM_TRUE;

    gl_context_ = emscripten_webgl_create_context(config.canvas_selector, &attrs);
    if (gl_context_ <= 0) {
        std::fprintf(stderr, "[Pictor/WebGL] Failed to create WebGL2 context on '%s' (err=%d)\n",
                     config.canvas_selector, static_cast<int>(gl_context_));
        return false;
    }

    EMSCRIPTEN_RESULT res = emscripten_webgl_make_context_current(gl_context_);
    if (res != EMSCRIPTEN_RESULT_SUCCESS) {
        std::fprintf(stderr, "[Pictor/WebGL] Failed to make context current (err=%d)\n", res);
        emscripten_webgl_destroy_context(gl_context_);
        gl_context_ = 0;
        return false;
    }

    // Query initial canvas size
    double css_w, css_h;
    emscripten_get_element_css_size(config.canvas_selector, &css_w, &css_h);
    width_  = static_cast<uint32_t>(css_w * dpr_);
    height_ = static_cast<uint32_t>(css_h * dpr_);

    // Set canvas drawing buffer size
    emscripten_set_canvas_element_size(config.canvas_selector, width_, height_);

    query_capabilities();

    // Default GL state
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    initialized_ = true;
    std::printf("[Pictor/WebGL] Initialized: %s (%ux%u @%.1fx)\n",
                caps_.renderer.c_str(), width_, height_, dpr_);
    return true;
}

void WebGLContext::shutdown() {
    if (!initialized_) return;

    if (gl_context_ > 0) {
        emscripten_webgl_destroy_context(gl_context_);
        gl_context_ = 0;
    }
    initialized_ = false;
}

void WebGLContext::resize(uint32_t width, uint32_t height) {
    width_  = width;
    height_ = height;
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
}

void WebGLContext::begin_frame(float r, float g, float b, float a) {
    glViewport(0, 0, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_));
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void WebGLContext::end_frame() {
    // WebGL: browser handles swapchain presentation via requestAnimationFrame.
    // Explicit flush to ensure commands are submitted.
    glFlush();
}

void WebGLContext::set_depth_test(bool enabled) {
    if (enabled) glEnable(GL_DEPTH_TEST);
    else         glDisable(GL_DEPTH_TEST);
}

void WebGLContext::set_cull_face(bool enabled) {
    if (enabled) glEnable(GL_CULL_FACE);
    else         glDisable(GL_CULL_FACE);
}

void WebGLContext::set_blend(bool enabled) {
    if (enabled) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
}

void WebGLContext::query_capabilities() {
    glGetIntegerv(GL_MAX_TEXTURE_SIZE,                &caps_.max_texture_size);
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &caps_.max_texture_units);
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS,              &caps_.max_vertex_attribs);
    glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS,     &caps_.max_uniform_buffer_bindings);
    glGetIntegerv(GL_MAX_DRAW_BUFFERS,                &caps_.max_draw_buffers);
    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS,           &caps_.max_color_attachments);
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE,           &caps_.max_renderbuffer_size);
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS,               caps_.max_viewport_dims);

    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* vendor   = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const char* version  = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* slang    = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));

    if (renderer) caps_.renderer = renderer;
    if (vendor)   caps_.vendor   = vendor;
    if (version)  caps_.version  = version;
    if (slang)    caps_.shading_language_version = slang;

    // Check extensions
    caps_.has_float_textures     = emscripten_webgl_enable_extension(gl_context_, "EXT_color_buffer_float");
    caps_.has_anisotropic_filter = emscripten_webgl_enable_extension(gl_context_, "EXT_texture_filter_anisotropic");
    caps_.has_parallel_compile   = emscripten_webgl_enable_extension(gl_context_, "KHR_parallel_shader_compile");
    caps_.has_multi_draw         = emscripten_webgl_enable_extension(gl_context_, "WEBGL_multi_draw");

    if (caps_.has_anisotropic_filter) {
        glGetFloatv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT */, &caps_.max_anisotropy);
    }
}

} // namespace pictor

#endif // PICTOR_HAS_WEBGL
