#pragma once

#ifdef PICTOR_HAS_WEBGL

// Web mesh module (SPEC-PC-WEB-MESH): draws indexed, vertex-coloured triangle meshes with a
// hemisphere ambient light, up to two directional lights and one metal/rough surface, on a
// WebGL2 canvas. Game-agnostic: the host owns the canvas, the camera maths and the frame loop.
//
// Ownership: the renderer owns its WebGL2 context, the shader program and every mesh it
// created (VAO + vertex buffer + index buffer). release_mesh / shutdown free them; a mesh
// handle from another renderer or a released handle is rejected.

#include "pictor/webgl/web_mesh_input.h"
#include "pictor/webgl/webgl_buffer.h"
#include "pictor/webgl/webgl_context.h"
#include "pictor/webgl/webgl_shader.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pictor {

/// 0 is never a valid mesh.
using WebMeshHandle = uint32_t;
constexpr WebMeshHandle INVALID_WEB_MESH = 0;

struct WebMeshRendererConfig {
    const char* canvas_selector = "#canvas";  ///< CSS selector of the host's <canvas>
    bool        antialias       = true;
};

/// Per-frame camera and target. Matrices are column-major 4x4.
struct WebFrame {
    uint32_t width  = 0;            ///< drawing-buffer pixels
    uint32_t height = 0;
    const float* view_projection = nullptr;  ///< 16 floats
    const float* camera_position = nullptr;  ///< 3 floats, world space (for specular)
    float clear_color[3] = {0.0f, 0.0f, 0.0f};  ///< sRGB 0..1
};

class WebMeshRenderer {
public:
    WebMeshRenderer();
    ~WebMeshRenderer();

    WebMeshRenderer(const WebMeshRenderer&) = delete;
    WebMeshRenderer& operator=(const WebMeshRenderer&) = delete;

    /// Creates the context and the program. False (with last_error) when WebGL2 is missing.
    bool initialize(const WebMeshRendererConfig& config);
    void shutdown();

    /// Uploads a mesh. INVALID_WEB_MESH (with last_error) when the input is rejected.
    WebMeshHandle create_mesh(const WebMeshInput& input, WebColorSpace color_space);
    /// Frees a mesh's GPU buffers. False for an unknown or already released handle.
    bool release_mesh(WebMeshHandle handle);

    bool set_lighting(const WebLighting& lighting);
    bool set_surface(const WebSurface& surface);

    /// begin_frame → submit* → end_frame. Calls out of this order fail with last_error.
    bool begin_frame(const WebFrame& frame);
    bool submit(WebMeshHandle handle, const float* model /* 16 floats, column-major */);
    bool end_frame();

    std::size_t live_mesh_count() const;
    const std::string& last_error() const { return last_error_; }

private:
    struct MeshEntry {
        GLuint            vao          = 0;
        WebGLBufferHandle vertex_buffer = INVALID_BUFFER;
        WebGLBufferHandle index_buffer  = INVALID_BUFFER;
        GLsizei           index_count  = 0;
        bool              alive        = false;
    };

    bool fail(const std::string& message);
    MeshEntry* find_live(WebMeshHandle handle);
    void destroy_entry(MeshEntry& entry);
    void upload_lighting();

    std::string        canvas_selector_;  // owned copy; the context keeps a pointer to it
    WebGLContext       context_;
    WebGLShaderManager shaders_;
    WebGLBufferManager buffers_;
    WebGLProgramHandle program_ = INVALID_PROGRAM;

    std::vector<MeshEntry>      meshes_;       // index = handle - 1
    std::vector<WebMeshHandle>  free_handles_;
    std::vector<float>          packing_;      // reused scratch for vertex packing

    WebLighting lighting_;
    WebSurface  surface_;
    bool        lighting_dirty_ = true;
    bool        in_frame_       = false;
    bool        initialized_    = false;
    std::string last_error_;
};

} // namespace pictor

#endif // PICTOR_HAS_WEBGL
