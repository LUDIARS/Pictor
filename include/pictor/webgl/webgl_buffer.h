#pragma once

#ifdef PICTOR_HAS_WEBGL

#include <GLES3/gl3.h>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace pictor {

/// Buffer usage hint for WebGL.
enum class WebGLBufferUsage : uint8_t {
    STATIC  = 0,  // GL_STATIC_DRAW  — uploaded once, drawn many times
    DYNAMIC = 1,  // GL_DYNAMIC_DRAW — updated frequently
    STREAM  = 2   // GL_STREAM_DRAW  — updated every frame
};

/// Handle to a managed GPU buffer.
using WebGLBufferHandle = uint32_t;
constexpr WebGLBufferHandle INVALID_BUFFER = 0;

/// Manages Vertex Buffer Objects (VBO), Index Buffer Objects (IBO),
/// and Uniform Buffer Objects (UBO) for the WebGL2 backend.
///
/// All buffers are tracked and released on shutdown.
/// Supports sub-data updates for dynamic/streaming buffers.
class WebGLBufferManager {
public:
    WebGLBufferManager();
    ~WebGLBufferManager();

    WebGLBufferManager(const WebGLBufferManager&) = delete;
    WebGLBufferManager& operator=(const WebGLBufferManager&) = delete;

    // ---- VBO (Vertex Buffer) ----

    /// Create a vertex buffer and optionally upload initial data.
    WebGLBufferHandle create_vertex_buffer(const void* data, size_t size_bytes,
                                           WebGLBufferUsage usage = WebGLBufferUsage::STATIC);

    /// Update a vertex buffer (full replace or sub-range).
    void update_vertex_buffer(WebGLBufferHandle handle, const void* data,
                              size_t size_bytes, size_t offset = 0);

    void bind_vertex_buffer(WebGLBufferHandle handle);

    // ---- IBO (Index Buffer) ----

    WebGLBufferHandle create_index_buffer(const void* data, size_t size_bytes,
                                          WebGLBufferUsage usage = WebGLBufferUsage::STATIC);

    void bind_index_buffer(WebGLBufferHandle handle);

    // ---- UBO (Uniform Buffer) ----

    WebGLBufferHandle create_uniform_buffer(size_t size_bytes,
                                            WebGLBufferUsage usage = WebGLBufferUsage::DYNAMIC);

    void update_uniform_buffer(WebGLBufferHandle handle, const void* data,
                               size_t size_bytes, size_t offset = 0);

    /// Bind a UBO to a binding point for use with uniform blocks.
    void bind_uniform_buffer_base(WebGLBufferHandle handle, GLuint binding_point);

    /// Bind a sub-range of a UBO to a binding point.
    void bind_uniform_buffer_range(WebGLBufferHandle handle, GLuint binding_point,
                                   size_t offset, size_t size);

    // ---- VAO (Vertex Array Object) ----

    /// Create an empty VAO. Returns the GL name.
    GLuint create_vao();

    void bind_vao(GLuint vao);
    void unbind_vao();
    void destroy_vao(GLuint vao);

    // ---- Lifecycle ----

    void destroy_buffer(WebGLBufferHandle handle);
    void shutdown();

    /// Get the underlying GL buffer name.
    GLuint gl_buffer(WebGLBufferHandle handle) const;

    /// Get buffer size in bytes.
    size_t buffer_size(WebGLBufferHandle handle) const;

private:
    struct BufferEntry {
        GLuint gl_id    = 0;
        GLenum target   = 0;       // GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER, GL_UNIFORM_BUFFER
        GLenum gl_usage = 0;       // GL_STATIC_DRAW, etc.
        size_t size     = 0;
    };

    static GLenum to_gl_usage(WebGLBufferUsage usage);
    WebGLBufferHandle allocate_entry(GLenum target, const void* data,
                                     size_t size_bytes, WebGLBufferUsage usage);

    std::vector<BufferEntry> buffers_;  // index 0 unused (INVALID_BUFFER)
    std::vector<GLuint>      vaos_;
};

} // namespace pictor

#endif // PICTOR_HAS_WEBGL
