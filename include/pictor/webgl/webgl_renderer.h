#pragma once

#ifdef PICTOR_HAS_WEBGL

#include "pictor/core/types.h"
#include "pictor/webgl/webgl_context.h"
#include "pictor/webgl/webgl_shader.h"
#include "pictor/webgl/webgl_buffer.h"
#include <cstdint>
#include <vector>

namespace pictor {

/// Per-frame statistics for the WebGL renderer.
struct WebGLFrameStats {
    float    fps              = 0.0f;
    float    frame_time_ms    = 0.0f;
    uint32_t draw_calls       = 0;
    uint32_t triangles        = 0;
    uint32_t visible_objects  = 0;
    uint32_t total_objects    = 0;
};

/// Configuration for WebGLRenderer initialization.
struct WebGLRendererConfig {
    WebGLContextConfig context_config;
    uint32_t           max_objects     = 100000;
    bool               enable_culling  = true;
    bool               enable_stats    = true;
};

/// Instance data packed for GPU upload.
/// Per-instance: 4x4 transform (64 bytes) + color (16 bytes) = 80 bytes.
struct WebGLInstanceData {
    float transform[16];  // column-major 4x4
    float color[4];       // RGBA
};

static_assert(sizeof(WebGLInstanceData) == 80, "WebGLInstanceData must be 80 bytes");

/// Minimal instanced renderer for the WebGL2 backend.
///
/// Parallels SimpleRenderer (Vulkan) in functionality:
///   - Generates an icosphere mesh
///   - Renders instances with per-instance transform + color
///   - Supports CPU frustum culling
///
/// Lifecycle:
///   initialize(config) -> [begin_frame -> render -> end_frame]* -> shutdown
///
/// This renderer is designed as a reference implementation and demo.
/// For production, integrate with PictorRenderer via the planned
/// RenderBackend trait abstraction.
class WebGLRenderer {
public:
    WebGLRenderer();
    ~WebGLRenderer();

    WebGLRenderer(const WebGLRenderer&) = delete;
    WebGLRenderer& operator=(const WebGLRenderer&) = delete;

    /// Initialize context, shaders, mesh, and buffers.
    bool initialize(const WebGLRendererConfig& config = {});

    /// Tear down all GL resources.
    void shutdown();

    /// Resize the viewport (call on canvas resize).
    void resize(uint32_t width, uint32_t height);

    // ---- Object Management ----

    /// Register an object. Returns object ID.
    ObjectId register_object(const ObjectDescriptor& desc);

    /// Remove an object.
    void unregister_object(ObjectId id);

    /// Update an object's transform.
    void update_transform(ObjectId id, const float4x4& transform);

    /// Update an object's color.
    void update_color(ObjectId id, float r, float g, float b, float a = 1.0f);

    // ---- Rendering ----

    /// Begin frame: clear buffers, reset stats.
    void begin_frame(float delta_time);

    /// Render with given camera matrices.
    /// view and projection are column-major 4x4.
    void render(const float* view, const float* projection);

    /// End frame (flush).
    void end_frame();

    // ---- Camera Helpers ----

    /// Set camera by look-at parameters.
    void set_camera(const float3& eye, const float3& target, const float3& up);

    /// Set perspective projection.
    void set_perspective(float fov_radians, float aspect, float near_plane, float far_plane);

    // ---- Query ----

    const WebGLFrameStats& stats() const { return stats_; }
    bool is_initialized() const { return initialized_; }
    uint32_t object_count() const { return active_count_; }

    WebGLContext&       context()       { return context_; }
    const WebGLContext& context() const { return context_; }

private:
    struct ObjectEntry {
        float4x4       transform;
        float          color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        AABB           bounds;
        MeshHandle     mesh     = INVALID_MESH;
        MaterialHandle material = INVALID_MATERIAL;
        uint16_t       flags    = 0;
        bool           active   = false;
    };

    struct Vertex {
        float pos[3];
        float normal[3];
    };

    bool create_shaders();
    bool create_mesh();
    bool create_instance_buffers();
    void generate_icosphere(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices);
    void upload_instances(const float* view_projection);

    // Math helpers
    static void mat4_multiply(float* out, const float* a, const float* b);
    static void mat4_look_at(float* out, const float3& eye, const float3& target, const float3& up);
    static void mat4_perspective(float* out, float fov, float aspect, float near_p, float far_p);

    // Subsystems
    WebGLContext        context_;
    WebGLShaderManager  shaders_;
    WebGLBufferManager  buffers_;

    // Shader programs
    WebGLProgramHandle  instanced_program_ = INVALID_PROGRAM;

    // Mesh (icosphere)
    WebGLBufferHandle   vertex_buffer_ = INVALID_BUFFER;
    WebGLBufferHandle   index_buffer_  = INVALID_BUFFER;
    GLuint              mesh_vao_      = 0;
    uint32_t            index_count_   = 0;

    // Instance buffer
    WebGLBufferHandle   instance_vbo_  = INVALID_BUFFER;

    // Objects
    std::vector<ObjectEntry> objects_;
    uint32_t                 active_count_ = 0;

    // Camera
    float view_matrix_[16]       = {};
    float projection_matrix_[16] = {};
    bool  camera_dirty_          = true;

    // Config
    WebGLRendererConfig config_;
    bool                enable_culling_ = true;

    // Stats
    WebGLFrameStats stats_;
    float           delta_time_   = 0.0f;
    bool            initialized_  = false;

    static constexpr uint32_t MAX_INSTANCES_PER_DRAW = 65536;
};

} // namespace pictor

#endif // PICTOR_HAS_WEBGL
