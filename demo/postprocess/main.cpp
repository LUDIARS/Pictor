/// Pictor Post-Process Demo
///
/// Demonstrates the post-processing pipeline with:
///   - HDR rendering with bright emissive objects (for bloom)
///   - Bloom: glowing light sources with configurable threshold
///   - Depth of Field: foreground/background blur with bokeh
///   - Tone Mapping: ACES, Reinhard, Uncharted2 operator comparison
///   - Gaussian Blur: configurable blur intensity
///   - Real-time effect toggling and parameter adjustment
///
/// Controls:
///   Mouse drag   — Orbit camera
///   Scroll       — Zoom in/out
///   1            — Toggle Bloom
///   2            — Toggle Depth of Field
///   3            — Toggle Gaussian Blur
///   4            — Cycle Tone Map operator (ACES → Reinhard → Uncharted2 → ...)
///   +/-          — Adjust exposure
///   B/N          — Adjust bloom threshold
///   F/G          — Adjust DoF focus distance
///   R            — Reset all settings to defaults
///   S            — Toggle stats overlay
///
/// Build target: pictor_postprocess_demo

#include "pictor/pictor.h"
#include "pictor/surface/vulkan_context.h"
#include "pictor/surface/glfw_surface_provider.h"
#include <GLFW/glfw3.h>

#include <cstdio>
#include <cmath>
#include <cstring>
#include <chrono>
#include <vector>
#include <string>

using namespace pictor;

// ============================================================
// Math Helpers
// ============================================================

namespace {

void mat4_identity(float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void mat4_look_at(float* out, const float* eye, const float* center, const float* up) {
    float fx = center[0] - eye[0], fy = center[1] - eye[1], fz = center[2] - eye[2];
    float fl = std::sqrt(fx*fx + fy*fy + fz*fz);
    fx /= fl; fy /= fl; fz /= fl;

    float sx = fy*up[2] - fz*up[1], sy = fz*up[0] - fx*up[2], sz = fx*up[1] - fy*up[0];
    float sl = std::sqrt(sx*sx + sy*sy + sz*sz);
    sx /= sl; sy /= sl; sz /= sl;

    float ux = sy*fz - sz*fy, uy = sz*fx - sx*fz, uz = sx*fy - sy*fx;

    mat4_identity(out);
    out[0] = sx;  out[4] = sy;  out[8]  = sz;  out[12] = -(sx*eye[0]+sy*eye[1]+sz*eye[2]);
    out[1] = ux;  out[5] = uy;  out[9]  = uz;  out[13] = -(ux*eye[0]+uy*eye[1]+uz*eye[2]);
    out[2] = -fx; out[6] = -fy; out[10] = -fz; out[14] = (fx*eye[0]+fy*eye[1]+fz*eye[2]);
    out[3] = 0;   out[7] = 0;   out[11] = 0;   out[15] = 1.0f;
}

void mat4_perspective(float* out, float fovy_rad, float aspect, float near_z, float far_z) {
    memset(out, 0, 16 * sizeof(float));
    float f = 1.0f / std::tan(fovy_rad * 0.5f);
    out[0]  = f / aspect;
    out[5]  = -f; // Vulkan Y-flip
    out[10] = far_z / (near_z - far_z);
    out[11] = -1.0f;
    out[14] = (near_z * far_z) / (near_z - far_z);
}

void mat4_multiply(float* out, const float* a, const float* b) {
    float tmp[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            tmp[j * 4 + i] = 0.0f;
            for (int k = 0; k < 4; k++)
                tmp[j * 4 + i] += a[k * 4 + i] * b[j * 4 + k];
        }
    memcpy(out, tmp, 16 * sizeof(float));
}

void mat4_scale(float* out, float sx, float sy, float sz) {
    mat4_identity(out);
    out[0] = sx; out[5] = sy; out[10] = sz;
}

void mat4_translation(float* out, float tx, float ty, float tz) {
    mat4_identity(out);
    out[12] = tx; out[13] = ty; out[14] = tz;
}

void mat4_rotation_y(float* out, float angle_rad) {
    mat4_identity(out);
    float c = std::cos(angle_rad), s = std::sin(angle_rad);
    out[0] = c; out[8] = s;
    out[2] = -s; out[10] = c;
}

const char* tonemap_operator_name(ToneMapOperator op) {
    switch (op) {
        case ToneMapOperator::ACES_FILMIC:  return "ACES Filmic";
        case ToneMapOperator::REINHARD:     return "Reinhard";
        case ToneMapOperator::REINHARD_EXT: return "Reinhard Extended";
        case ToneMapOperator::UNCHARTED2:   return "Uncharted 2 (Hable)";
        case ToneMapOperator::LINEAR_CLAMP: return "Linear (no tonemap)";
        default: return "Unknown";
    }
}

} // anonymous namespace

// ============================================================
// Scene Data Structures
// ============================================================

struct SceneUBO {
    float view[16];
    float proj[16];
    float viewProj[16];
    float cameraPos[4];
    float ambientColor[4];
    float sunDirection[4];
    float sunColor[4];
    float time;
    float pad0, pad1, pad2;
};

struct InstanceData {
    float model[16];
    float baseColor[4];
    float pbrParams[4];
    float emissiveColor[4];
};

// ============================================================
// Mesh Generation
// ============================================================

struct Vertex {
    float pos[3];
    float normal[3];
};

static void generate_cube(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices) {
    const float P = 0.5f, N = -0.5f;
    struct FV { float x, y, z, nx, ny, nz; };
    const FV fd[] = {
        {N,N,P,0,0,1},{P,N,P,0,0,1},{P,P,P,0,0,1},{N,P,P,0,0,1},
        {P,N,N,0,0,-1},{N,N,N,0,0,-1},{N,P,N,0,0,-1},{P,P,N,0,0,-1},
        {P,N,P,1,0,0},{P,N,N,1,0,0},{P,P,N,1,0,0},{P,P,P,1,0,0},
        {N,N,N,-1,0,0},{N,N,P,-1,0,0},{N,P,P,-1,0,0},{N,P,N,-1,0,0},
        {N,P,P,0,1,0},{P,P,P,0,1,0},{P,P,N,0,1,0},{N,P,N,0,1,0},
        {N,N,N,0,-1,0},{P,N,N,0,-1,0},{P,N,P,0,-1,0},{N,N,P,0,-1,0},
    };
    vertices.clear(); indices.clear();
    for (int i = 0; i < 24; i++) {
        Vertex v; v.pos[0]=fd[i].x; v.pos[1]=fd[i].y; v.pos[2]=fd[i].z;
        v.normal[0]=fd[i].nx; v.normal[1]=fd[i].ny; v.normal[2]=fd[i].nz;
        vertices.push_back(v);
    }
    for (int f = 0; f < 6; f++) {
        uint32_t b = f * 4;
        indices.push_back(b); indices.push_back(b+1); indices.push_back(b+2);
        indices.push_back(b); indices.push_back(b+2); indices.push_back(b+3);
    }
}

static void generate_ground_plane(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                                   float half_size = 15.0f) {
    vertices.clear(); indices.clear();
    Vertex v0={{-half_size,0,-half_size},{0,1,0}}, v1={{half_size,0,-half_size},{0,1,0}};
    Vertex v2={{half_size,0,half_size},{0,1,0}}, v3={{-half_size,0,half_size},{0,1,0}};
    vertices.push_back(v0); vertices.push_back(v1);
    vertices.push_back(v2); vertices.push_back(v3);
    indices.push_back(0); indices.push_back(1); indices.push_back(2);
    indices.push_back(0); indices.push_back(2); indices.push_back(3);
}

static void generate_sphere(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                              float radius = 0.5f, uint32_t slices = 32, uint32_t stacks = 24) {
    vertices.clear(); indices.clear();
    const float pi = 3.14159265f;
    for (uint32_t j = 0; j <= stacks; ++j) {
        float phi = pi * float(j) / float(stacks);
        float sp = std::sin(phi), cp = std::cos(phi);
        for (uint32_t i = 0; i <= slices; ++i) {
            float theta = 2.0f * pi * float(i) / float(slices);
            float st = std::sin(theta), ct = std::cos(theta);
            float nx = sp*ct, ny = cp, nz = sp*st;
            Vertex v; v.pos[0]=radius*nx; v.pos[1]=radius*ny; v.pos[2]=radius*nz;
            v.normal[0]=nx; v.normal[1]=ny; v.normal[2]=nz;
            vertices.push_back(v);
        }
    }
    for (uint32_t j = 0; j < stacks; ++j)
        for (uint32_t i = 0; i < slices; ++i) {
            uint32_t a = j*(slices+1)+i, b = a+slices+1;
            indices.push_back(a); indices.push_back(b); indices.push_back(a+1);
            indices.push_back(a+1); indices.push_back(b); indices.push_back(b+1);
        }
}

// ============================================================
// Post-Process Demo State
// ============================================================

struct DemoState {
    PostProcessConfig pp_config;
    int tonemap_index = 0;

    // Orbit camera
    float yaw    = 0.4f;
    float pitch  = 0.35f;
    float radius = 14.0f;
    float center[3] = {0.0f, 1.5f, 0.0f};
    double lastMouseX = 0.0, lastMouseY = 0.0;
    bool dragging = false;
};
static DemoState g_state;

// ============================================================
// Main
// ============================================================

int main() {
    printf("=== Pictor Post-Process Demo ===\n\n");

    // ---- 1. GLFW Window ----
    GlfwSurfaceProvider surface_provider;
    GlfwWindowConfig win_cfg;
    win_cfg.width  = 1920;
    win_cfg.height = 1080;
    win_cfg.title  = "Pictor — Post-Process Demo (Bloom / DoF / ToneMapping / Blur)";
    win_cfg.vsync  = true;

    if (!surface_provider.create(win_cfg)) {
        fprintf(stderr, "Failed to create GLFW window\n");
        return 1;
    }

    // ---- 2. Vulkan Context ----
    VulkanContext vk_ctx;
    VulkanContextConfig vk_cfg;
    vk_cfg.app_name   = "Pictor PostProcess Demo";
    vk_cfg.validation = true;

    if (!vk_ctx.initialize(&surface_provider, vk_cfg)) {
        fprintf(stderr, "Failed to initialize Vulkan context\n");
        surface_provider.destroy();
        return 1;
    }

    uint32_t screen_w = 1920, screen_h = 1080;
#ifdef PICTOR_HAS_VULKAN
    screen_w = vk_ctx.swapchain_extent().width;
    screen_h = vk_ctx.swapchain_extent().height;
#endif
    printf("Vulkan initialized: %ux%u\n", screen_w, screen_h);

    // ---- 3. Pictor Renderer ----
    RendererConfig pictor_cfg;
    pictor_cfg.initial_profile  = "Standard";
    pictor_cfg.screen_width     = screen_w;
    pictor_cfg.screen_height    = screen_h;
    pictor_cfg.profiler_enabled = true;
    pictor_cfg.overlay_mode     = OverlayMode::STANDARD;

    PictorRenderer renderer;
    renderer.initialize(pictor_cfg);

    // ---- 4. Configure Post-Process Pipeline ----
    PostProcessConfig& pp = g_state.pp_config;

    // HDR
    pp.hdr.enabled       = true;
    pp.hdr.exposure      = 1.0f;
    pp.hdr.gamma         = 2.2f;
    pp.hdr.auto_exposure = false;

    // Bloom: moderately aggressive for HDR demo
    pp.bloom.enabled        = true;
    pp.bloom.threshold      = 1.0f;
    pp.bloom.soft_threshold = 0.5f;
    pp.bloom.intensity      = 0.8f;
    pp.bloom.radius         = 5.0f;
    pp.bloom.mip_levels     = 5;
    pp.bloom.scatter        = 0.7f;

    // DoF: focus on center of scene
    pp.depth_of_field.enabled        = true;
    pp.depth_of_field.focus_distance = 12.0f;
    pp.depth_of_field.focus_range    = 4.0f;
    pp.depth_of_field.bokeh_radius   = 4.0f;
    pp.depth_of_field.near_start     = 0.0f;
    pp.depth_of_field.near_end       = 4.0f;
    pp.depth_of_field.far_start      = 18.0f;
    pp.depth_of_field.far_end        = 40.0f;
    pp.depth_of_field.sample_count   = 16;

    // Gaussian Blur: disabled by default (toggle with key)
    pp.gaussian_blur.enabled     = false;
    pp.gaussian_blur.sigma       = 2.0f;
    pp.gaussian_blur.kernel_size = 9;
    pp.gaussian_blur.separable   = true;
    pp.gaussian_blur.intensity   = 1.0f;

    // Tone Mapping: ACES Filmic (industry standard)
    pp.tone_mapping.enabled    = true;
    pp.tone_mapping.op         = ToneMapOperator::ACES_FILMIC;
    pp.tone_mapping.exposure   = 1.0f;
    pp.tone_mapping.gamma      = 2.2f;
    pp.tone_mapping.white_point = 4.0f;
    pp.tone_mapping.saturation = 1.0f;

    renderer.set_postprocess_config(pp);

    // ---- 5. Register Scene Objects ----
    // Central emissive cube (very bright — triggers bloom)
    {
        ObjectDescriptor desc;
        desc.mesh = 0; desc.material = 0;
        desc.transform = float4x4::identity();
        desc.transform.set_translation(0.0f, 2.0f, 0.0f);
        desc.bounds.min = {-1.5f, 0.5f, -1.5f};
        desc.bounds.max = { 1.5f, 3.5f,  1.5f};
        desc.flags = ObjectFlags::STATIC | ObjectFlags::CAST_SHADOW;
        renderer.register_object(desc);
    }

    // Ground plane
    {
        ObjectDescriptor desc;
        desc.mesh = 1; desc.material = 1;
        desc.transform = float4x4::identity();
        desc.bounds.min = {-15.0f, -0.01f, -15.0f};
        desc.bounds.max = { 15.0f,  0.01f,  15.0f};
        desc.flags = ObjectFlags::STATIC | ObjectFlags::RECEIVE_SHADOW;
        renderer.register_object(desc);
    }

    // Emissive light spheres (scattered around scene for bloom demo)
    struct LightSphere { float x, z, r, g, b, intensity; };
    LightSphere light_spheres[] = {
        { 5.0f,  3.0f,  1.0f, 0.2f, 0.1f, 8.0f},   // Red-orange
        {-4.0f,  5.0f,  0.1f, 0.5f, 1.0f, 6.0f},   // Cyan
        { 2.0f, -4.0f,  1.0f, 0.8f, 0.0f, 10.0f},  // Yellow (very bright!)
        {-6.0f, -2.0f,  0.6f, 0.1f, 1.0f, 5.0f},   // Purple
        { 7.0f, -1.0f,  0.0f, 1.0f, 0.3f, 7.0f},   // Green
    };

    for (int i = 0; i < 5; ++i) {
        ObjectDescriptor desc;
        desc.mesh = 2; desc.material = static_cast<MaterialHandle>(2 + i);
        desc.transform = float4x4::identity();
        desc.transform.set_translation(light_spheres[i].x, 1.0f, light_spheres[i].z);
        desc.bounds.min = {light_spheres[i].x - 0.5f, 0.5f, light_spheres[i].z - 0.5f};
        desc.bounds.max = {light_spheres[i].x + 0.5f, 1.5f, light_spheres[i].z + 0.5f};
        desc.flags = ObjectFlags::STATIC;
        renderer.register_object(desc);
    }

    // Foreground object (for DoF near blur)
    {
        ObjectDescriptor desc;
        desc.mesh = 0; desc.material = 7;
        desc.transform = float4x4::identity();
        desc.transform.set_translation(2.0f, 0.5f, 8.0f);
        desc.bounds.min = {1.5f, 0.0f, 7.5f};
        desc.bounds.max = {2.5f, 1.0f, 8.5f};
        desc.flags = ObjectFlags::STATIC | ObjectFlags::CAST_SHADOW;
        renderer.register_object(desc);
    }

    // Background objects (for DoF far blur)
    for (int i = 0; i < 3; ++i) {
        ObjectDescriptor desc;
        desc.mesh = 2; desc.material = 8;
        desc.transform = float4x4::identity();
        float bx = -5.0f + i * 5.0f;
        desc.transform.set_translation(bx, 1.2f, -10.0f);
        desc.bounds.min = {bx - 1.0f, 0.0f, -11.0f};
        desc.bounds.max = {bx + 1.0f, 2.4f, -9.0f};
        desc.flags = ObjectFlags::STATIC | ObjectFlags::CAST_SHADOW;
        renderer.register_object(desc);
    }

    // Dynamic orbiting emissive sphere
    ObjectId orbit_sphere_id;
    {
        ObjectDescriptor desc;
        desc.mesh = 2; desc.material = 9;
        desc.transform = float4x4::identity();
        desc.transform.set_translation(6.0f, 2.0f, 0.0f);
        desc.bounds.min = {5.5f, 1.5f, -0.5f};
        desc.bounds.max = {6.5f, 2.5f,  0.5f};
        desc.flags = ObjectFlags::DYNAMIC;
        orbit_sphere_id = renderer.register_object(desc);
    }

    // ---- 6. Camera ----
    Camera camera;
    camera.position = {0.0f, 5.0f, 14.0f};
    for (int i = 0; i < 6; ++i) {
        camera.frustum.planes[i].normal   = {0, 0, 0};
        camera.frustum.planes[i].distance = 10000.0f;
    }
    camera.frustum.planes[0] = {{1, 0, 0}, 50.0f};
    camera.frustum.planes[1] = {{-1, 0, 0}, 50.0f};
    camera.frustum.planes[2] = {{0, 1, 0}, 50.0f};
    camera.frustum.planes[3] = {{0, -1, 0}, 50.0f};
    camera.frustum.planes[4] = {{0, 0, 1}, 0.1f};
    camera.frustum.planes[5] = {{0, 0, -1}, 200.0f};

    // ---- 7. Input Callbacks ----
    GLFWwindow* win = surface_provider.glfw_window();

    glfwSetMouseButtonCallback(win, [](GLFWwindow* w, int button, int action, int) {
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
            g_state.dragging = (action == GLFW_PRESS);
            if (g_state.dragging)
                glfwGetCursorPos(w, &g_state.lastMouseX, &g_state.lastMouseY);
        }
    });

    glfwSetCursorPosCallback(win, [](GLFWwindow*, double xpos, double ypos) {
        if (!g_state.dragging) return;
        double dx = xpos - g_state.lastMouseX;
        double dy = ypos - g_state.lastMouseY;
        g_state.lastMouseX = xpos;
        g_state.lastMouseY = ypos;
        g_state.yaw   -= float(dx) * 0.005f;
        g_state.pitch += float(dy) * 0.005f;
        g_state.pitch = std::clamp(g_state.pitch, -0.2f, 1.5f);
    });

    glfwSetScrollCallback(win, [](GLFWwindow*, double, double yoffset) {
        g_state.radius -= float(yoffset) * 1.5f;
        g_state.radius = std::clamp(g_state.radius, 3.0f, 50.0f);
    });

    glfwSetKeyCallback(win, [](GLFWwindow*, int key, int, int action, int) {
        if (action != GLFW_PRESS) return;
        auto& pp = g_state.pp_config;

        switch (key) {
            case GLFW_KEY_1:
                pp.bloom.enabled = !pp.bloom.enabled;
                printf("[PostProcess] Bloom: %s\n", pp.bloom.enabled ? "ON" : "OFF");
                break;
            case GLFW_KEY_2:
                pp.depth_of_field.enabled = !pp.depth_of_field.enabled;
                printf("[PostProcess] DoF: %s\n", pp.depth_of_field.enabled ? "ON" : "OFF");
                break;
            case GLFW_KEY_3:
                pp.gaussian_blur.enabled = !pp.gaussian_blur.enabled;
                printf("[PostProcess] Gaussian Blur: %s\n", pp.gaussian_blur.enabled ? "ON" : "OFF");
                break;
            case GLFW_KEY_4: {
                g_state.tonemap_index = (g_state.tonemap_index + 1) % 5;
                pp.tone_mapping.op = static_cast<ToneMapOperator>(g_state.tonemap_index);
                printf("[PostProcess] Tone Map: %s\n", tonemap_operator_name(pp.tone_mapping.op));
                break;
            }
            case GLFW_KEY_EQUAL: // +
                pp.tone_mapping.exposure += 0.1f;
                printf("[PostProcess] Exposure: %.2f\n", pp.tone_mapping.exposure);
                break;
            case GLFW_KEY_MINUS: // -
                pp.tone_mapping.exposure = std::max(0.1f, pp.tone_mapping.exposure - 0.1f);
                printf("[PostProcess] Exposure: %.2f\n", pp.tone_mapping.exposure);
                break;
            case GLFW_KEY_B:
                pp.bloom.threshold = std::max(0.1f, pp.bloom.threshold - 0.1f);
                printf("[PostProcess] Bloom threshold: %.2f\n", pp.bloom.threshold);
                break;
            case GLFW_KEY_N:
                pp.bloom.threshold += 0.1f;
                printf("[PostProcess] Bloom threshold: %.2f\n", pp.bloom.threshold);
                break;
            case GLFW_KEY_F:
                pp.depth_of_field.focus_distance = std::max(1.0f, pp.depth_of_field.focus_distance - 1.0f);
                printf("[PostProcess] DoF focus: %.1f\n", pp.depth_of_field.focus_distance);
                break;
            case GLFW_KEY_G:
                pp.depth_of_field.focus_distance += 1.0f;
                printf("[PostProcess] DoF focus: %.1f\n", pp.depth_of_field.focus_distance);
                break;
            case GLFW_KEY_R:
                g_state.pp_config = PostProcessConfig{};
                g_state.pp_config.bloom.enabled = true;
                g_state.pp_config.tone_mapping.enabled = true;
                g_state.tonemap_index = 0;
                printf("[PostProcess] Reset to defaults\n");
                break;
        }
    });

    // ---- 8. Main Loop ----
    auto start = std::chrono::high_resolution_clock::now();
    uint64_t frame_count = 0;

    printf("\nPost-Process Demo Scene:\n");
    printf("  - Central emissive cube (bright HDR — bloom source)\n");
    printf("  - 5 colored emissive light spheres (bloom sources)\n");
    printf("  - Foreground cube (near DoF blur)\n");
    printf("  - Background spheres (far DoF blur)\n");
    printf("  - Orbiting emissive sphere (dynamic bloom)\n");
    printf("\nControls:\n");
    printf("  1 = Toggle Bloom     2 = Toggle DoF\n");
    printf("  3 = Toggle Blur      4 = Cycle ToneMap\n");
    printf("  +/- = Exposure       B/N = Bloom threshold\n");
    printf("  F/G = DoF focus      R = Reset\n");
    printf("  Mouse: orbit camera  Scroll: zoom\n");
    printf("\nEntering main loop. Close window to exit.\n\n");

    while (!surface_provider.should_close()) {
        surface_provider.poll_events();

        auto now = std::chrono::high_resolution_clock::now();
        float elapsed = std::chrono::duration<float>(now - start).count();

        // Update post-process config each frame (in case keys changed it)
        renderer.set_postprocess_config(g_state.pp_config);

        // Orbit camera
        float cos_pitch = std::cos(g_state.pitch);
        float eye[3] = {
            g_state.center[0] + g_state.radius * cos_pitch * std::sin(g_state.yaw),
            g_state.center[1] + g_state.radius * std::sin(g_state.pitch),
            g_state.center[2] + g_state.radius * cos_pitch * std::cos(g_state.yaw)
        };
        float up[3] = {0.0f, 1.0f, 0.0f};

        float view_mat[16], proj_mat[16], view_proj[16];
        mat4_look_at(view_mat, eye, g_state.center, up);
        float aspect = float(screen_w) / float(screen_h);
        mat4_perspective(proj_mat, 0.7854f, aspect, 0.1f, 200.0f);
        mat4_multiply(view_proj, proj_mat, view_mat);

        // Dynamic orbiting emissive sphere
        float orbit_radius = 6.0f;
        float orbit_speed  = 0.6f;
        float dyn_x = orbit_radius * std::sin(elapsed * orbit_speed);
        float dyn_z = orbit_radius * std::cos(elapsed * orbit_speed);
        float dyn_y = 2.0f + 0.5f * std::sin(elapsed * 1.2f);

        {
            float4x4 dyn_transform = float4x4::identity();
            dyn_transform.set_translation(dyn_x, dyn_y, dyn_z);
            renderer.update_transform(orbit_sphere_id, dyn_transform);
        }

#ifdef PICTOR_HAS_VULKAN
        uint32_t image_idx = vk_ctx.acquire_next_image();
        if (image_idx == UINT32_MAX) continue;

        auto cmd = vk_ctx.command_buffers()[image_idx];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &begin_info);

        // Clear with dark background (HDR scene rendering would go here)
        VkClearValue clear_values[2];
        clear_values[0].color = {{0.01f, 0.01f, 0.02f, 1.0f}};
        clear_values[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rp_info{};
        rp_info.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_info.renderPass  = vk_ctx.default_render_pass();
        rp_info.framebuffer = vk_ctx.framebuffers()[image_idx];
        rp_info.renderArea  = {{0, 0}, vk_ctx.swapchain_extent()};
        rp_info.clearValueCount = 2;
        rp_info.pClearValues    = clear_values;

        vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(cmd);

        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphore wait_sem  = vk_ctx.image_available_semaphore();
        VkSemaphore sig_sem   = vk_ctx.render_finished_semaphore();

        VkSubmitInfo submit_info{};
        submit_info.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount   = 1;
        submit_info.pWaitSemaphores      = &wait_sem;
        submit_info.pWaitDstStageMask    = &wait_stage;
        submit_info.commandBufferCount   = 1;
        submit_info.pCommandBuffers      = &cmd;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores    = &sig_sem;

        vkQueueSubmit(vk_ctx.graphics_queue(), 1, &submit_info, vk_ctx.in_flight_fence());
        vk_ctx.present(image_idx);
#endif

        // Run Pictor data pipeline (includes post-process execution)
        float dt = 1.0f / 60.0f;
        camera.position = {eye[0], eye[1], eye[2]};
        renderer.begin_frame(dt);
        renderer.render(camera);
        renderer.end_frame();

        ++frame_count;

        // Print stats periodically
        if (frame_count % 180 == 0) {
            const auto& stats = renderer.get_frame_stats();
            const auto& ppc = g_state.pp_config;
            printf("[Frame %llu] FPS: %.1f  PostProcess: Bloom=%s DoF=%s Blur=%s ToneMap=%s(%s) Exp=%.2f\n",
                   static_cast<unsigned long long>(frame_count),
                   stats.fps,
                   ppc.bloom.enabled ? "ON" : "OFF",
                   ppc.depth_of_field.enabled ? "ON" : "OFF",
                   ppc.gaussian_blur.enabled ? "ON" : "OFF",
                   ppc.tone_mapping.enabled ? "ON" : "OFF",
                   tonemap_operator_name(ppc.tone_mapping.op),
                   ppc.tone_mapping.exposure);
        }
    }

    // ---- 9. Cleanup ----
    vk_ctx.device_wait_idle();
    renderer.shutdown();
    vk_ctx.shutdown();
    surface_provider.destroy();

    printf("\nPost-process demo finished. %llu frames rendered.\n",
           static_cast<unsigned long long>(frame_count));
    return 0;
}
