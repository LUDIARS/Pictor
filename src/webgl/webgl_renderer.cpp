#ifdef PICTOR_HAS_WEBGL

#include "pictor/webgl/webgl_renderer.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace pictor {

// ============================================================
// Embedded GLSL ES 3.00 shaders
// ============================================================

static const char* k_instanced_vert = R"(#version 300 es
precision highp float;

// Per-vertex
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;

// Per-instance (mat4 = 4 x vec4, location 2..5)
layout(location = 2) in vec4 a_model_0;
layout(location = 3) in vec4 a_model_1;
layout(location = 4) in vec4 a_model_2;
layout(location = 5) in vec4 a_model_3;

// Per-instance color
layout(location = 6) in vec4 a_color;

uniform mat4 u_view_projection;

out vec3 v_world_normal;
out vec3 v_world_pos;
out vec4 v_color;

void main() {
    mat4 model = mat4(a_model_0, a_model_1, a_model_2, a_model_3);
    vec4 world_pos = model * vec4(a_position, 1.0);
    gl_Position = u_view_projection * world_pos;

    // Normal transform (upper-left 3x3 of model — assumes uniform scale)
    v_world_normal = mat3(model) * a_normal;
    v_world_pos    = world_pos.xyz;
    v_color        = a_color;
}
)";

static const char* k_instanced_frag = R"(#version 300 es
precision highp float;

in vec3 v_world_normal;
in vec3 v_world_pos;
in vec4 v_color;

out vec4 frag_color;

const vec3 LIGHT_DIR   = normalize(vec3(0.5, 1.0, 0.3));
const vec3 LIGHT_COLOR = vec3(1.0, 0.98, 0.95);
const vec3 AMBIENT     = vec3(0.15, 0.17, 0.22);

void main() {
    vec3 N = normalize(v_world_normal);
    float NdotL = max(dot(N, LIGHT_DIR), 0.0);

    // Simple half-Lambert for softer shading
    float half_lambert = NdotL * 0.5 + 0.5;

    vec3 diffuse = v_color.rgb * LIGHT_COLOR * half_lambert;
    vec3 color   = AMBIENT * v_color.rgb + diffuse;

    // Simple rim light
    vec3 view_dir = normalize(-v_world_pos);
    float rim = 1.0 - max(dot(N, view_dir), 0.0);
    rim = smoothstep(0.6, 1.0, rim) * 0.3;
    color += vec3(rim);

    frag_color = vec4(color, v_color.a);
}
)";

// ============================================================
// Constructor / Destructor
// ============================================================

WebGLRenderer::WebGLRenderer() {
    // Identity matrices
    std::memset(view_matrix_, 0, sizeof(view_matrix_));
    std::memset(projection_matrix_, 0, sizeof(projection_matrix_));
    view_matrix_[0] = view_matrix_[5] = view_matrix_[10] = view_matrix_[15] = 1.0f;
    projection_matrix_[0] = projection_matrix_[5] = projection_matrix_[10] = projection_matrix_[15] = 1.0f;
}

WebGLRenderer::~WebGLRenderer() {
    if (initialized_) shutdown();
}

// ============================================================
// Lifecycle
// ============================================================

bool WebGLRenderer::initialize(const WebGLRendererConfig& config) {
    if (initialized_) return true;
    config_ = config;
    enable_culling_ = config.enable_culling;

    if (!context_.initialize(config.context_config)) {
        std::fprintf(stderr, "[Pictor/WebGLRenderer] Context initialization failed\n");
        return false;
    }

    if (!create_shaders()) {
        std::fprintf(stderr, "[Pictor/WebGLRenderer] Shader creation failed\n");
        context_.shutdown();
        return false;
    }

    if (!create_mesh()) {
        std::fprintf(stderr, "[Pictor/WebGLRenderer] Mesh creation failed\n");
        shaders_.shutdown();
        context_.shutdown();
        return false;
    }

    if (!create_instance_buffers()) {
        std::fprintf(stderr, "[Pictor/WebGLRenderer] Instance buffer creation failed\n");
        shaders_.shutdown();
        context_.shutdown();
        return false;
    }

    objects_.reserve(config.max_objects);
    initialized_ = true;
    std::printf("[Pictor/WebGLRenderer] Initialized (max_objects=%u)\n", config.max_objects);
    return true;
}

void WebGLRenderer::shutdown() {
    if (!initialized_) return;

    if (mesh_vao_ != 0) {
        buffers_.destroy_vao(mesh_vao_);
        mesh_vao_ = 0;
    }

    buffers_.shutdown();
    shaders_.shutdown();
    context_.shutdown();

    objects_.clear();
    active_count_ = 0;
    initialized_ = false;
}

void WebGLRenderer::resize(uint32_t width, uint32_t height) {
    context_.resize(width, height);
}

// ============================================================
// Object Management
// ============================================================

ObjectId WebGLRenderer::register_object(const ObjectDescriptor& desc) {
    ObjectEntry entry;
    entry.transform = desc.transform;
    entry.bounds    = desc.bounds;
    entry.mesh      = desc.mesh;
    entry.material  = desc.material;
    entry.flags     = desc.flags;
    entry.active    = true;
    // Default white color
    entry.color[0] = 1.0f; entry.color[1] = 1.0f;
    entry.color[2] = 1.0f; entry.color[3] = 1.0f;

    // Find a free slot or append
    for (uint32_t i = 0; i < static_cast<uint32_t>(objects_.size()); ++i) {
        if (!objects_[i].active) {
            objects_[i] = entry;
            ++active_count_;
            return i;
        }
    }

    auto id = static_cast<ObjectId>(objects_.size());
    objects_.push_back(entry);
    ++active_count_;
    return id;
}

void WebGLRenderer::unregister_object(ObjectId id) {
    if (id >= objects_.size() || !objects_[id].active) return;
    objects_[id].active = false;
    --active_count_;
}

void WebGLRenderer::update_transform(ObjectId id, const float4x4& transform) {
    if (id >= objects_.size() || !objects_[id].active) return;
    objects_[id].transform = transform;
}

void WebGLRenderer::update_color(ObjectId id, float r, float g, float b, float a) {
    if (id >= objects_.size() || !objects_[id].active) return;
    objects_[id].color[0] = r;
    objects_[id].color[1] = g;
    objects_[id].color[2] = b;
    objects_[id].color[3] = a;
}

// ============================================================
// Rendering
// ============================================================

void WebGLRenderer::begin_frame(float delta_time) {
    delta_time_ = delta_time;
    stats_ = {};
    context_.begin_frame(0.08f, 0.08f, 0.10f, 1.0f);
}

void WebGLRenderer::render(const float* view, const float* projection) {
    if (active_count_ == 0) return;

    // Use provided matrices or fall back to internal camera
    const float* v = view       ? view       : view_matrix_;
    const float* p = projection ? projection : projection_matrix_;

    // Compute VP matrix
    float vp[16];
    mat4_multiply(vp, p, v);

    // Collect visible instances
    upload_instances(vp);

    if (stats_.visible_objects == 0) return;

    // Bind shader and set VP uniform
    shaders_.use(instanced_program_);
    shaders_.set_uniform_mat4(instanced_program_, "u_view_projection", vp);

    // Bind VAO (mesh + instance attributes already configured)
    buffers_.bind_vao(mesh_vao_);

    // Issue instanced draw
    glDrawElementsInstanced(GL_TRIANGLES,
                            static_cast<GLsizei>(index_count_),
                            GL_UNSIGNED_INT,
                            nullptr,
                            static_cast<GLsizei>(stats_.visible_objects));

    stats_.draw_calls  = 1;
    stats_.triangles   = (index_count_ / 3) * stats_.visible_objects;

    buffers_.unbind_vao();
    shaders_.unbind();
}

void WebGLRenderer::end_frame() {
    context_.end_frame();

    stats_.total_objects = active_count_;
    if (delta_time_ > 0.0f) {
        stats_.fps = 1.0f / delta_time_;
    }
    stats_.frame_time_ms = delta_time_ * 1000.0f;
}

// ============================================================
// Camera
// ============================================================

void WebGLRenderer::set_camera(const float3& eye, const float3& target, const float3& up) {
    mat4_look_at(view_matrix_, eye, target, up);
    camera_dirty_ = true;
}

void WebGLRenderer::set_perspective(float fov_radians, float aspect, float near_plane, float far_plane) {
    mat4_perspective(projection_matrix_, fov_radians, aspect, near_plane, far_plane);
    camera_dirty_ = true;
}

// ============================================================
// Internal: Shader/Mesh/Buffer Setup
// ============================================================

bool WebGLRenderer::create_shaders() {
    instanced_program_ = shaders_.create_program(k_instanced_vert, k_instanced_frag, "instanced");
    return instanced_program_ != INVALID_PROGRAM;
}

bool WebGLRenderer::create_mesh() {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    generate_icosphere(vertices, indices);
    index_count_ = static_cast<uint32_t>(indices.size());

    vertex_buffer_ = buffers_.create_vertex_buffer(
        vertices.data(), vertices.size() * sizeof(Vertex), WebGLBufferUsage::STATIC);
    index_buffer_ = buffers_.create_index_buffer(
        indices.data(), indices.size() * sizeof(uint32_t), WebGLBufferUsage::STATIC);

    if (vertex_buffer_ == INVALID_BUFFER || index_buffer_ == INVALID_BUFFER)
        return false;

    // Create VAO
    mesh_vao_ = buffers_.create_vao();
    if (mesh_vao_ == 0) return false;

    buffers_.bind_vao(mesh_vao_);

    // Per-vertex attributes
    buffers_.bind_vertex_buffer(vertex_buffer_);
    // location 0: position (vec3)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<const void*>(offsetof(Vertex, pos)));
    // location 1: normal (vec3)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<const void*>(offsetof(Vertex, normal)));

    // Index buffer
    buffers_.bind_index_buffer(index_buffer_);

    buffers_.unbind_vao();
    return true;
}

bool WebGLRenderer::create_instance_buffers() {
    // Allocate instance VBO (resized dynamically)
    size_t initial_size = 1024 * sizeof(WebGLInstanceData);
    instance_vbo_ = buffers_.create_vertex_buffer(nullptr, initial_size, WebGLBufferUsage::STREAM);
    return instance_vbo_ != INVALID_BUFFER;
}

void WebGLRenderer::upload_instances(const float* /*view_projection*/) {
    // Build instance data array
    std::vector<WebGLInstanceData> instances;
    instances.reserve(active_count_);

    for (const auto& obj : objects_) {
        if (!obj.active) continue;

        // TODO: frustum culling using view_projection

        WebGLInstanceData inst;
        // Column-major: copy transform rows → columns
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                inst.transform[col * 4 + row] = obj.transform.m[col][row];
            }
        }
        std::memcpy(inst.color, obj.color, sizeof(inst.color));
        instances.push_back(inst);

        if (instances.size() >= MAX_INSTANCES_PER_DRAW) break;
    }

    stats_.visible_objects = static_cast<uint32_t>(instances.size());
    if (instances.empty()) return;

    // Upload instance data
    size_t data_size = instances.size() * sizeof(WebGLInstanceData);
    buffers_.update_vertex_buffer(instance_vbo_, instances.data(), data_size);

    // Configure per-instance attributes in the VAO
    buffers_.bind_vao(mesh_vao_);
    buffers_.bind_vertex_buffer(instance_vbo_);

    // Per-instance transform: mat4 as 4 x vec4 at locations 2..5
    for (int i = 0; i < 4; ++i) {
        GLuint loc = static_cast<GLuint>(2 + i);
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE,
                              sizeof(WebGLInstanceData),
                              reinterpret_cast<const void*>(
                                  static_cast<size_t>(i) * 4 * sizeof(float)));
        glVertexAttribDivisor(loc, 1);
    }

    // Per-instance color at location 6
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE,
                          sizeof(WebGLInstanceData),
                          reinterpret_cast<const void*>(offsetof(WebGLInstanceData, color)));
    glVertexAttribDivisor(6, 1);

    buffers_.unbind_vao();
}

// ============================================================
// Icosphere Generation
// ============================================================

void WebGLRenderer::generate_icosphere(std::vector<Vertex>& vertices,
                                        std::vector<uint32_t>& indices) {
    // Initial icosahedron
    const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;

    auto add_vertex = [&](float x, float y, float z) {
        float len = std::sqrt(x * x + y * y + z * z);
        Vertex v;
        v.pos[0] = x / len; v.pos[1] = y / len; v.pos[2] = z / len;
        v.normal[0] = v.pos[0]; v.normal[1] = v.pos[1]; v.normal[2] = v.pos[2];
        vertices.push_back(v);
    };

    add_vertex(-1,  t,  0); add_vertex( 1,  t,  0);
    add_vertex(-1, -t,  0); add_vertex( 1, -t,  0);
    add_vertex( 0, -1,  t); add_vertex( 0,  1,  t);
    add_vertex( 0, -1, -t); add_vertex( 0,  1, -t);
    add_vertex( t,  0, -1); add_vertex( t,  0,  1);
    add_vertex(-t,  0, -1); add_vertex(-t,  0,  1);

    // 20 faces of icosahedron
    uint32_t faces[] = {
        0,11,5,  0,5,1,   0,1,7,   0,7,10,  0,10,11,
        1,5,9,   5,11,4,  11,10,2, 10,7,6,  7,1,8,
        3,9,4,   3,4,2,   3,2,6,   3,6,8,   3,8,9,
        4,9,5,   2,4,11,  6,2,10,  8,6,7,   9,8,1,
    };

    // One level of subdivision for a smoother sphere
    struct Triangle { uint32_t a, b, c; };
    std::vector<Triangle> tris;
    for (size_t i = 0; i < 60; i += 3) {
        tris.push_back({faces[i], faces[i+1], faces[i+2]});
    }

    auto midpoint = [&](uint32_t i0, uint32_t i1) -> uint32_t {
        float mx = (vertices[i0].pos[0] + vertices[i1].pos[0]) * 0.5f;
        float my = (vertices[i0].pos[1] + vertices[i1].pos[1]) * 0.5f;
        float mz = (vertices[i0].pos[2] + vertices[i1].pos[2]) * 0.5f;
        float len = std::sqrt(mx*mx + my*my + mz*mz);
        Vertex v;
        v.pos[0] = mx/len; v.pos[1] = my/len; v.pos[2] = mz/len;
        v.normal[0] = v.pos[0]; v.normal[1] = v.pos[1]; v.normal[2] = v.pos[2];
        auto idx = static_cast<uint32_t>(vertices.size());
        vertices.push_back(v);
        return idx;
    };

    // Subdivide twice for ~320 triangles
    for (int sub = 0; sub < 2; ++sub) {
        std::vector<Triangle> new_tris;
        new_tris.reserve(tris.size() * 4);
        for (const auto& tri : tris) {
            uint32_t ab = midpoint(tri.a, tri.b);
            uint32_t bc = midpoint(tri.b, tri.c);
            uint32_t ca = midpoint(tri.c, tri.a);
            new_tris.push_back({tri.a, ab, ca});
            new_tris.push_back({tri.b, bc, ab});
            new_tris.push_back({tri.c, ca, bc});
            new_tris.push_back({ab, bc, ca});
        }
        tris = std::move(new_tris);
    }

    indices.reserve(tris.size() * 3);
    for (const auto& tri : tris) {
        indices.push_back(tri.a);
        indices.push_back(tri.b);
        indices.push_back(tri.c);
    }
}

// ============================================================
// Math Helpers
// ============================================================

void WebGLRenderer::mat4_multiply(float* out, const float* a, const float* b) {
    float tmp[16];
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            tmp[col * 4 + row] =
                a[0 * 4 + row] * b[col * 4 + 0] +
                a[1 * 4 + row] * b[col * 4 + 1] +
                a[2 * 4 + row] * b[col * 4 + 2] +
                a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
    std::memcpy(out, tmp, sizeof(tmp));
}

void WebGLRenderer::mat4_look_at(float* out, const float3& eye, const float3& target, const float3& up) {
    float3 f = target - eye;
    float fl = std::sqrt(f.x*f.x + f.y*f.y + f.z*f.z);
    f = f * (1.0f / fl);

    // right = normalize(cross(f, up))
    float3 r;
    r.x = f.y * up.z - f.z * up.y;
    r.y = f.z * up.x - f.x * up.z;
    r.z = f.x * up.y - f.y * up.x;
    float rl = std::sqrt(r.x*r.x + r.y*r.y + r.z*r.z);
    r = r * (1.0f / rl);

    // u = cross(r, f)
    float3 u;
    u.x = r.y * f.z - r.z * f.y;
    u.y = r.z * f.x - r.x * f.z;
    u.z = r.x * f.y - r.y * f.x;

    // Column-major
    out[ 0] = r.x;  out[ 1] = u.x;  out[ 2] = -f.x; out[ 3] = 0.0f;
    out[ 4] = r.y;  out[ 5] = u.y;  out[ 6] = -f.y; out[ 7] = 0.0f;
    out[ 8] = r.z;  out[ 9] = u.z;  out[10] = -f.z; out[11] = 0.0f;
    out[12] = -(r.x*eye.x + r.y*eye.y + r.z*eye.z);
    out[13] = -(u.x*eye.x + u.y*eye.y + u.z*eye.z);
    out[14] =  (f.x*eye.x + f.y*eye.y + f.z*eye.z);
    out[15] = 1.0f;
}

void WebGLRenderer::mat4_perspective(float* out, float fov, float aspect, float near_p, float far_p) {
    float f = 1.0f / std::tan(fov * 0.5f);
    float nf = 1.0f / (near_p - far_p);

    std::memset(out, 0, 16 * sizeof(float));
    out[ 0] = f / aspect;
    out[ 5] = f;
    out[10] = (far_p + near_p) * nf;
    out[11] = -1.0f;
    out[14] = 2.0f * far_p * near_p * nf;
}

} // namespace pictor

#endif // PICTOR_HAS_WEBGL
