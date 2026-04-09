#ifdef PICTOR_HAS_WEBGL

#include "pictor/webgl/webgl_buffer.h"
#include <cstdio>

namespace pictor {

WebGLBufferManager::WebGLBufferManager() {
    buffers_.push_back(BufferEntry{}); // sentinel at index 0
}

WebGLBufferManager::~WebGLBufferManager() {
    shutdown();
}

GLenum WebGLBufferManager::to_gl_usage(WebGLBufferUsage usage) {
    switch (usage) {
        case WebGLBufferUsage::STATIC:  return GL_STATIC_DRAW;
        case WebGLBufferUsage::DYNAMIC: return GL_DYNAMIC_DRAW;
        case WebGLBufferUsage::STREAM:  return GL_STREAM_DRAW;
        default: return GL_STATIC_DRAW;
    }
}

WebGLBufferHandle WebGLBufferManager::allocate_entry(GLenum target, const void* data,
                                                      size_t size_bytes, WebGLBufferUsage usage) {
    GLuint gl_id = 0;
    glGenBuffers(1, &gl_id);
    if (gl_id == 0) return INVALID_BUFFER;

    GLenum gl_usage = to_gl_usage(usage);

    glBindBuffer(target, gl_id);
    glBufferData(target, static_cast<GLsizeiptr>(size_bytes), data, gl_usage);
    glBindBuffer(target, 0);

    BufferEntry entry;
    entry.gl_id    = gl_id;
    entry.target   = target;
    entry.gl_usage = gl_usage;
    entry.size     = size_bytes;

    auto handle = static_cast<WebGLBufferHandle>(buffers_.size());
    buffers_.push_back(entry);
    return handle;
}

// ---- VBO ----

WebGLBufferHandle WebGLBufferManager::create_vertex_buffer(const void* data, size_t size_bytes,
                                                            WebGLBufferUsage usage) {
    return allocate_entry(GL_ARRAY_BUFFER, data, size_bytes, usage);
}

void WebGLBufferManager::update_vertex_buffer(WebGLBufferHandle handle, const void* data,
                                               size_t size_bytes, size_t offset) {
    if (handle == INVALID_BUFFER || handle >= buffers_.size()) return;
    auto& entry = buffers_[handle];
    glBindBuffer(GL_ARRAY_BUFFER, entry.gl_id);
    if (offset == 0 && size_bytes >= entry.size) {
        // Full replace — may re-allocate for better driver behavior
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(size_bytes), data, entry.gl_usage);
        entry.size = size_bytes;
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(offset),
                        static_cast<GLsizeiptr>(size_bytes), data);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void WebGLBufferManager::bind_vertex_buffer(WebGLBufferHandle handle) {
    if (handle == INVALID_BUFFER || handle >= buffers_.size()) return;
    glBindBuffer(GL_ARRAY_BUFFER, buffers_[handle].gl_id);
}

// ---- IBO ----

WebGLBufferHandle WebGLBufferManager::create_index_buffer(const void* data, size_t size_bytes,
                                                           WebGLBufferUsage usage) {
    return allocate_entry(GL_ELEMENT_ARRAY_BUFFER, data, size_bytes, usage);
}

void WebGLBufferManager::bind_index_buffer(WebGLBufferHandle handle) {
    if (handle == INVALID_BUFFER || handle >= buffers_.size()) return;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers_[handle].gl_id);
}

// ---- UBO ----

WebGLBufferHandle WebGLBufferManager::create_uniform_buffer(size_t size_bytes,
                                                             WebGLBufferUsage usage) {
    return allocate_entry(GL_UNIFORM_BUFFER, nullptr, size_bytes, usage);
}

void WebGLBufferManager::update_uniform_buffer(WebGLBufferHandle handle, const void* data,
                                                size_t size_bytes, size_t offset) {
    if (handle == INVALID_BUFFER || handle >= buffers_.size()) return;
    auto& entry = buffers_[handle];
    glBindBuffer(GL_UNIFORM_BUFFER, entry.gl_id);
    glBufferSubData(GL_UNIFORM_BUFFER, static_cast<GLintptr>(offset),
                    static_cast<GLsizeiptr>(size_bytes), data);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void WebGLBufferManager::bind_uniform_buffer_base(WebGLBufferHandle handle, GLuint binding_point) {
    if (handle == INVALID_BUFFER || handle >= buffers_.size()) return;
    glBindBufferBase(GL_UNIFORM_BUFFER, binding_point, buffers_[handle].gl_id);
}

void WebGLBufferManager::bind_uniform_buffer_range(WebGLBufferHandle handle, GLuint binding_point,
                                                    size_t offset, size_t size) {
    if (handle == INVALID_BUFFER || handle >= buffers_.size()) return;
    glBindBufferRange(GL_UNIFORM_BUFFER, binding_point, buffers_[handle].gl_id,
                      static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size));
}

// ---- VAO ----

GLuint WebGLBufferManager::create_vao() {
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    if (vao != 0) vaos_.push_back(vao);
    return vao;
}

void WebGLBufferManager::bind_vao(GLuint vao) {
    glBindVertexArray(vao);
}

void WebGLBufferManager::unbind_vao() {
    glBindVertexArray(0);
}

void WebGLBufferManager::destroy_vao(GLuint vao) {
    if (vao == 0) return;
    glDeleteVertexArrays(1, &vao);
    for (auto it = vaos_.begin(); it != vaos_.end(); ++it) {
        if (*it == vao) { vaos_.erase(it); break; }
    }
}

// ---- Lifecycle ----

void WebGLBufferManager::destroy_buffer(WebGLBufferHandle handle) {
    if (handle == INVALID_BUFFER || handle >= buffers_.size()) return;
    auto& entry = buffers_[handle];
    if (entry.gl_id != 0) {
        glDeleteBuffers(1, &entry.gl_id);
        entry.gl_id = 0;
        entry.size  = 0;
    }
}

void WebGLBufferManager::shutdown() {
    for (size_t i = 1; i < buffers_.size(); ++i) {
        if (buffers_[i].gl_id != 0) {
            glDeleteBuffers(1, &buffers_[i].gl_id);
        }
    }
    buffers_.clear();
    buffers_.push_back(BufferEntry{}); // restore sentinel

    for (GLuint vao : vaos_) {
        if (vao != 0) glDeleteVertexArrays(1, &vao);
    }
    vaos_.clear();
}

GLuint WebGLBufferManager::gl_buffer(WebGLBufferHandle handle) const {
    if (handle == INVALID_BUFFER || handle >= buffers_.size()) return 0;
    return buffers_[handle].gl_id;
}

size_t WebGLBufferManager::buffer_size(WebGLBufferHandle handle) const {
    if (handle == INVALID_BUFFER || handle >= buffers_.size()) return 0;
    return buffers_[handle].size;
}

} // namespace pictor

#endif // PICTOR_HAS_WEBGL
