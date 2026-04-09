#ifdef PICTOR_HAS_WEBGL

#include "pictor/webgl/webgl_shader.h"
#include <cstdio>

namespace pictor {

WebGLShaderManager::WebGLShaderManager() {
    // Reserve index 0 as invalid sentinel
    programs_.push_back(ProgramEntry{});
}

WebGLShaderManager::~WebGLShaderManager() {
    shutdown();
}

WebGLProgramHandle WebGLShaderManager::create_program(const char* vert_source,
                                                       const char* frag_source,
                                                       const char* debug_name) {
    const char* label = debug_name ? debug_name : "unnamed";

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_source, label);
    if (vs == 0) return INVALID_PROGRAM;

    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_source, label);
    if (fs == 0) {
        glDeleteShader(vs);
        return INVALID_PROGRAM;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    // Shaders can be deleted after linking
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint link_status = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &link_status);
    if (link_status == GL_FALSE) {
        GLint log_len = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
        std::string log(static_cast<size_t>(log_len), '\0');
        glGetProgramInfoLog(program, log_len, nullptr, log.data());
        std::fprintf(stderr, "[Pictor/WebGL] Program link failed '%s':\n%s\n",
                     label, log.c_str());
        glDeleteProgram(program);
        return INVALID_PROGRAM;
    }

    ProgramEntry entry;
    entry.gl_id = program;
    entry.name  = label;

    auto handle = static_cast<WebGLProgramHandle>(programs_.size());
    programs_.push_back(std::move(entry));
    return handle;
}

void WebGLShaderManager::destroy_program(WebGLProgramHandle handle) {
    if (handle == INVALID_PROGRAM || handle >= programs_.size()) return;
    auto& entry = programs_[handle];
    if (entry.gl_id != 0) {
        glDeleteProgram(entry.gl_id);
        entry.gl_id = 0;
        entry.uniform_cache.clear();
    }
}

void WebGLShaderManager::shutdown() {
    for (size_t i = 1; i < programs_.size(); ++i) {
        if (programs_[i].gl_id != 0) {
            glDeleteProgram(programs_[i].gl_id);
        }
    }
    programs_.clear();
    programs_.push_back(ProgramEntry{}); // restore sentinel
    current_ = INVALID_PROGRAM;
}

void WebGLShaderManager::use(WebGLProgramHandle handle) {
    if (handle == current_) return;
    if (handle == INVALID_PROGRAM || handle >= programs_.size()) return;
    glUseProgram(programs_[handle].gl_id);
    current_ = handle;
}

void WebGLShaderManager::unbind() {
    glUseProgram(0);
    current_ = INVALID_PROGRAM;
}

GLint WebGLShaderManager::get_uniform_location(WebGLProgramHandle handle, const char* name) {
    if (handle == INVALID_PROGRAM || handle >= programs_.size()) return -1;
    auto& entry = programs_[handle];

    auto it = entry.uniform_cache.find(name);
    if (it != entry.uniform_cache.end()) return it->second;

    GLint loc = glGetUniformLocation(entry.gl_id, name);
    entry.uniform_cache[name] = loc;
    return loc;
}

void WebGLShaderManager::set_uniform_1i(WebGLProgramHandle handle, const char* name, int value) {
    GLint loc = get_uniform_location(handle, name);
    if (loc >= 0) glUniform1i(loc, value);
}

void WebGLShaderManager::set_uniform_1f(WebGLProgramHandle handle, const char* name, float value) {
    GLint loc = get_uniform_location(handle, name);
    if (loc >= 0) glUniform1f(loc, value);
}

void WebGLShaderManager::set_uniform_2f(WebGLProgramHandle handle, const char* name, float x, float y) {
    GLint loc = get_uniform_location(handle, name);
    if (loc >= 0) glUniform2f(loc, x, y);
}

void WebGLShaderManager::set_uniform_3f(WebGLProgramHandle handle, const char* name, float x, float y, float z) {
    GLint loc = get_uniform_location(handle, name);
    if (loc >= 0) glUniform3f(loc, x, y, z);
}

void WebGLShaderManager::set_uniform_4f(WebGLProgramHandle handle, const char* name, float x, float y, float z, float w) {
    GLint loc = get_uniform_location(handle, name);
    if (loc >= 0) glUniform4f(loc, x, y, z, w);
}

void WebGLShaderManager::set_uniform_mat4(WebGLProgramHandle handle, const char* name, const float* matrix) {
    GLint loc = get_uniform_location(handle, name);
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, matrix);
}

void WebGLShaderManager::bind_uniform_block(WebGLProgramHandle handle, const char* block_name, GLuint binding_point) {
    if (handle == INVALID_PROGRAM || handle >= programs_.size()) return;
    GLuint idx = glGetUniformBlockIndex(programs_[handle].gl_id, block_name);
    if (idx != GL_INVALID_INDEX) {
        glUniformBlockBinding(programs_[handle].gl_id, idx, binding_point);
    }
}

GLuint WebGLShaderManager::gl_program(WebGLProgramHandle handle) const {
    if (handle == INVALID_PROGRAM || handle >= programs_.size()) return 0;
    return programs_[handle].gl_id;
}

GLuint WebGLShaderManager::compile_shader(GLenum type, const char* source, const char* debug_name) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compile_status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compile_status);
    if (compile_status == GL_FALSE) {
        GLint log_len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
        std::string log(static_cast<size_t>(log_len), '\0');
        glGetShaderInfoLog(shader, log_len, nullptr, log.data());
        const char* type_str = (type == GL_VERTEX_SHADER) ? "vertex" : "fragment";
        std::fprintf(stderr, "[Pictor/WebGL] Shader compile failed '%s' (%s):\n%s\n",
                     debug_name, type_str, log.c_str());
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace pictor

#endif // PICTOR_HAS_WEBGL
