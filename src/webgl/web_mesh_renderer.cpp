#ifdef PICTOR_HAS_WEBGL

#include "pictor/webgl/web_mesh_renderer.h"
#include "web_mesh_shaders.h"

#include <emscripten/html5.h>
#include <cmath>
#include <cstddef>

namespace pictor {

namespace {

constexpr GLuint ATTRIB_POSITION = 0;
constexpr GLuint ATTRIB_NORMAL   = 1;
constexpr GLuint ATTRIB_COLOR    = 2;
constexpr GLsizei VERTEX_STRIDE  = static_cast<GLsizei>(WEB_MESH_VERTEX_FLOATS * sizeof(float));

void linear_rgb(const float (&srgb)[3], float scale, float (&out)[3]) {
    for (int c = 0; c < 3; ++c) out[c] = srgb_to_linear(srgb[c]) * scale;
}

bool all_finite(const float* values, int count) {
    for (int i = 0; i < count; ++i) {
        if (!std::isfinite(values[i])) return false;
    }
    return true;
}

} // namespace

WebMeshRenderer::WebMeshRenderer() = default;

WebMeshRenderer::~WebMeshRenderer() {
    shutdown();
}

bool WebMeshRenderer::fail(const std::string& message) {
    last_error_ = message;
    return false;
}

bool WebMeshRenderer::initialize(const WebMeshRendererConfig& config) {
    if (initialized_) return fail("already initialized");
    if (!config.canvas_selector || config.canvas_selector[0] == '\0') return fail("canvas selector is empty");
    canvas_selector_ = config.canvas_selector;

    WebGLContextConfig context_config;
    context_config.canvas_selector = canvas_selector_.c_str();
    context_config.antialias = config.antialias;
    if (!context_.initialize(context_config)) return fail("WebGL2 context could not be created");

    program_ = shaders_.create_program(web_mesh_shaders::VERTEX, web_mesh_shaders::FRAGMENT, "web_mesh_lit");
    if (program_ == INVALID_PROGRAM) {
        // The context exists already; give it back before reporting.
        context_.shutdown();
        return fail("web mesh shader failed to compile or link");
    }
    initialized_ = true;
    lighting_dirty_ = true;
    last_error_.clear();
    return true;
}

void WebMeshRenderer::shutdown() {
    if (!initialized_) return;
    for (MeshEntry& entry : meshes_) {
        if (entry.alive) destroy_entry(entry);
    }
    meshes_.clear();
    free_handles_.clear();
    shaders_.shutdown();
    buffers_.shutdown();
    context_.shutdown();
    program_ = INVALID_PROGRAM;
    in_frame_ = false;
    initialized_ = false;
}

WebMeshRenderer::MeshEntry* WebMeshRenderer::find_live(WebMeshHandle handle) {
    if (handle == INVALID_WEB_MESH || handle > meshes_.size()) return nullptr;
    MeshEntry& entry = meshes_[handle - 1];
    return entry.alive ? &entry : nullptr;
}

void WebMeshRenderer::destroy_entry(MeshEntry& entry) {
    buffers_.destroy_vao(entry.vao);
    buffers_.destroy_buffer(entry.vertex_buffer);
    buffers_.destroy_buffer(entry.index_buffer);
    entry = MeshEntry{};
}

WebMeshHandle WebMeshRenderer::create_mesh(const WebMeshInput& input, WebColorSpace color_space) {
    if (!initialized_) {
        fail("renderer is not initialized");
        return INVALID_WEB_MESH;
    }
    const WebMeshInputError error = validate_web_mesh_input(input);
    if (error != WebMeshInputError::NONE) {
        fail(std::string("invalid mesh: ") + web_mesh_input_error_name(error));
        return INVALID_WEB_MESH;
    }
    pack_web_mesh_vertices(input, color_space, packing_);

    MeshEntry entry;
    entry.vertex_buffer = buffers_.create_vertex_buffer(packing_.data(), packing_.size() * sizeof(float));
    entry.index_buffer  = buffers_.create_index_buffer(input.indices, input.index_count * sizeof(uint32_t));
    entry.vao = buffers_.create_vao();
    if (entry.vertex_buffer == INVALID_BUFFER || entry.index_buffer == INVALID_BUFFER || entry.vao == 0) {
        // Partial allocation: release what was created before reporting.
        if (entry.vao != 0) buffers_.destroy_vao(entry.vao);
        if (entry.vertex_buffer != INVALID_BUFFER) buffers_.destroy_buffer(entry.vertex_buffer);
        if (entry.index_buffer != INVALID_BUFFER) buffers_.destroy_buffer(entry.index_buffer);
        fail("GPU buffers could not be allocated");
        return INVALID_WEB_MESH;
    }
    buffers_.bind_vao(entry.vao);
    buffers_.bind_vertex_buffer(entry.vertex_buffer);
    buffers_.bind_index_buffer(entry.index_buffer);
    glEnableVertexAttribArray(ATTRIB_POSITION);
    glVertexAttribPointer(ATTRIB_POSITION, 3, GL_FLOAT, GL_FALSE, VERTEX_STRIDE, reinterpret_cast<const void*>(0));
    glEnableVertexAttribArray(ATTRIB_NORMAL);
    glVertexAttribPointer(ATTRIB_NORMAL, 3, GL_FLOAT, GL_FALSE, VERTEX_STRIDE, reinterpret_cast<const void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(ATTRIB_COLOR);
    glVertexAttribPointer(ATTRIB_COLOR, 3, GL_FLOAT, GL_FALSE, VERTEX_STRIDE, reinterpret_cast<const void*>(6 * sizeof(float)));
    buffers_.unbind_vao();

    entry.index_count = static_cast<GLsizei>(input.index_count);
    entry.alive = true;

    WebMeshHandle handle;
    if (!free_handles_.empty()) {
        handle = free_handles_.back();
        free_handles_.pop_back();
        meshes_[handle - 1] = entry;
    } else {
        meshes_.push_back(entry);
        handle = static_cast<WebMeshHandle>(meshes_.size());
    }
    return handle;
}

bool WebMeshRenderer::release_mesh(WebMeshHandle handle) {
    MeshEntry* entry = find_live(handle);
    if (!entry) return fail("unknown or released mesh");
    destroy_entry(*entry);
    free_handles_.push_back(handle);
    return true;
}

bool WebMeshRenderer::set_lighting(const WebLighting& lighting) {
    const WebLightingError error = validate_web_lighting(lighting);
    if (error != WebLightingError::NONE) return fail(std::string("invalid lighting: ") + web_lighting_error_name(error));
    lighting_ = lighting;
    lighting_dirty_ = true;
    return true;
}

bool WebMeshRenderer::set_surface(const WebSurface& surface) {
    const WebLightingError error = validate_web_surface(surface);
    if (error != WebLightingError::NONE) return fail(std::string("invalid surface: ") + web_lighting_error_name(error));
    surface_ = surface;
    lighting_dirty_ = true;
    return true;
}

void WebMeshRenderer::upload_lighting() {
    float sky[3];
    float ground[3];
    linear_rgb(lighting_.sky_color, lighting_.hemisphere_intensity, sky);
    linear_rgb(lighting_.ground_color, lighting_.hemisphere_intensity, ground);
    shaders_.set_uniform_3f(program_, "u_sky", sky[0], sky[1], sky[2]);
    shaders_.set_uniform_3f(program_, "u_ground", ground[0], ground[1], ground[2]);
    shaders_.set_uniform_1i(program_, "u_light_count", static_cast<int>(lighting_.directional_count));

    float directions[WEB_MAX_DIRECTIONAL_LIGHTS * 3] = {};
    float radiance[WEB_MAX_DIRECTIONAL_LIGHTS * 3] = {};
    for (uint32_t i = 0; i < lighting_.directional_count; ++i) {
        const WebDirectionalLight& light = lighting_.directional[i];
        const float length = std::sqrt(light.direction[0] * light.direction[0] +
                                       light.direction[1] * light.direction[1] +
                                       light.direction[2] * light.direction[2]);
        float color[3];
        linear_rgb(light.color, light.intensity, color);
        for (int c = 0; c < 3; ++c) {
            directions[i * 3 + c] = light.direction[c] / length;
            radiance[i * 3 + c] = color[c];
        }
    }
    glUniform3fv(shaders_.get_uniform_location(program_, "u_light_direction"),
                 static_cast<GLsizei>(WEB_MAX_DIRECTIONAL_LIGHTS), directions);
    glUniform3fv(shaders_.get_uniform_location(program_, "u_light_radiance"),
                 static_cast<GLsizei>(WEB_MAX_DIRECTIONAL_LIGHTS), radiance);
    shaders_.set_uniform_1f(program_, "u_roughness", surface_.roughness);
    shaders_.set_uniform_1f(program_, "u_metalness", surface_.metalness);
    lighting_dirty_ = false;
}

bool WebMeshRenderer::begin_frame(const WebFrame& frame) {
    if (!initialized_) return fail("renderer is not initialized");
    if (in_frame_) return fail("begin_frame called twice without end_frame");
    if (frame.width == 0 || frame.height == 0) return fail("frame size must be positive");
    if (!frame.view_projection || !frame.camera_position) return fail("frame matrices are missing");
    if (!all_finite(frame.view_projection, 16) || !all_finite(frame.camera_position, 3) ||
        !all_finite(frame.clear_color, 3)) {
        return fail("frame values must be finite");
    }
    if (frame.width != context_.width() || frame.height != context_.height()) {
        emscripten_set_canvas_element_size(canvas_selector_.c_str(), static_cast<int>(frame.width),
                                           static_cast<int>(frame.height));
        context_.resize(frame.width, frame.height);
    }
    // The default framebuffer is not an sRGB target and glClear bypasses the shader's encode,
    // so the sRGB clear colour is written as given.
    context_.begin_frame(frame.clear_color[0], frame.clear_color[1], frame.clear_color[2], 1.0f);

    shaders_.use(program_);
    if (lighting_dirty_) upload_lighting();
    shaders_.set_uniform_mat4(program_, "u_view_projection", frame.view_projection);
    shaders_.set_uniform_3f(program_, "u_camera_position", frame.camera_position[0],
                            frame.camera_position[1], frame.camera_position[2]);
    in_frame_ = true;
    return true;
}

bool WebMeshRenderer::submit(WebMeshHandle handle, const float* model) {
    if (!in_frame_) return fail("submit outside begin_frame/end_frame");
    MeshEntry* entry = find_live(handle);
    if (!entry) return fail("unknown or released mesh");
    if (!model || !all_finite(model, 16)) return fail("model matrix must be 16 finite floats");
    shaders_.set_uniform_mat4(program_, "u_model", model);
    buffers_.bind_vao(entry->vao);
    glDrawElements(GL_TRIANGLES, entry->index_count, GL_UNSIGNED_INT, nullptr);
    buffers_.unbind_vao();
    return true;
}

bool WebMeshRenderer::end_frame() {
    if (!in_frame_) return fail("end_frame without begin_frame");
    shaders_.unbind();
    context_.end_frame();
    in_frame_ = false;
    return true;
}

std::size_t WebMeshRenderer::live_mesh_count() const {
    std::size_t count = 0;
    for (const MeshEntry& entry : meshes_) {
        if (entry.alive) ++count;
    }
    return count;
}

} // namespace pictor

#endif // PICTOR_HAS_WEBGL
