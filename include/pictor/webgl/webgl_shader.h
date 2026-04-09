#pragma once

#ifdef PICTOR_HAS_WEBGL

#include <GLES3/gl3.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pictor {

/// Handle to a compiled shader program.
using WebGLProgramHandle = uint32_t;
constexpr WebGLProgramHandle INVALID_PROGRAM = 0;

/// Vertex attribute binding description for VAO setup.
struct WebGLAttribBinding {
    GLuint      location;
    GLint       size;       // 1..4
    GLenum      type;       // GL_FLOAT, GL_UNSIGNED_BYTE, etc.
    GLboolean   normalized; // GL_TRUE for UNORM
    GLsizei     stride;
    const void* offset;     // byte offset cast to void*
    GLuint      divisor;    // 0 = per-vertex, 1 = per-instance
};

/// Manages compilation, linking, caching, and uniform lookup of
/// GLSL ES 3.00 shader programs for the WebGL2 backend.
///
/// Typical usage:
///   auto handle = shader_mgr.create_program(vert_src, frag_src, "basic");
///   shader_mgr.use(handle);
///   shader_mgr.set_uniform_mat4(handle, "u_view_projection", vp);
class WebGLShaderManager {
public:
    WebGLShaderManager();
    ~WebGLShaderManager();

    WebGLShaderManager(const WebGLShaderManager&) = delete;
    WebGLShaderManager& operator=(const WebGLShaderManager&) = delete;

    /// Compile and link a vertex+fragment shader pair.
    /// Returns program handle, or INVALID_PROGRAM on failure.
    WebGLProgramHandle create_program(const char* vert_source,
                                      const char* frag_source,
                                      const char* debug_name = nullptr);

    /// Destroy a program and free GL resources.
    void destroy_program(WebGLProgramHandle handle);

    /// Destroy all programs.
    void shutdown();

    /// Bind the program for rendering.
    void use(WebGLProgramHandle handle);

    /// Unbind (bind program 0).
    void unbind();

    // ---- Uniform setters ----

    GLint get_uniform_location(WebGLProgramHandle handle, const char* name);

    void set_uniform_1i(WebGLProgramHandle handle, const char* name, int value);
    void set_uniform_1f(WebGLProgramHandle handle, const char* name, float value);
    void set_uniform_2f(WebGLProgramHandle handle, const char* name, float x, float y);
    void set_uniform_3f(WebGLProgramHandle handle, const char* name, float x, float y, float z);
    void set_uniform_4f(WebGLProgramHandle handle, const char* name, float x, float y, float z, float w);
    void set_uniform_mat4(WebGLProgramHandle handle, const char* name, const float* matrix);

    /// Bind a UBO to a named uniform block.
    void bind_uniform_block(WebGLProgramHandle handle, const char* block_name, GLuint binding_point);

    /// Get the underlying GL program ID (for advanced use).
    GLuint gl_program(WebGLProgramHandle handle) const;

private:
    struct ProgramEntry {
        GLuint      gl_id = 0;
        std::string name;
        std::unordered_map<std::string, GLint> uniform_cache;
    };

    GLuint compile_shader(GLenum type, const char* source, const char* debug_name);

    std::vector<ProgramEntry> programs_;       // index 0 unused (INVALID_PROGRAM)
    WebGLProgramHandle        current_ = INVALID_PROGRAM;
};

} // namespace pictor

#endif // PICTOR_HAS_WEBGL
