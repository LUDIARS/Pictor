#ifdef PICTOR_HAS_WEBGL

#include "pictor/webgl/webgl_renderer.h"
#include <emscripten.h>
#include <emscripten/html5.h>
#include <cmath>
#include <cstdio>

using namespace pictor;

// Global renderer (Emscripten callbacks require global/static state)
static WebGLRenderer g_renderer;
static double        g_last_time = 0.0;
static float         g_elapsed   = 0.0f;

// ---- Animation loop (called by requestAnimationFrame) ----

static void main_loop() {
    double now = emscripten_get_now() / 1000.0;  // seconds
    float dt = static_cast<float>(now - g_last_time);
    g_last_time = now;
    g_elapsed += dt;

    g_renderer.begin_frame(dt);

    // Orbit camera around the scene
    float radius = 8.0f;
    float cam_x = std::cos(g_elapsed * 0.3f) * radius;
    float cam_z = std::sin(g_elapsed * 0.3f) * radius;
    float cam_y = 3.0f + std::sin(g_elapsed * 0.5f) * 1.5f;

    float3 eye    = {cam_x, cam_y, cam_z};
    float3 target = {0.0f, 0.0f, 0.0f};
    float3 up     = {0.0f, 1.0f, 0.0f};

    g_renderer.set_camera(eye, target, up);

    float aspect = static_cast<float>(g_renderer.context().width()) /
                   static_cast<float>(g_renderer.context().height());
    g_renderer.set_perspective(0.785f, aspect, 0.1f, 100.0f);

    // Update dynamic object transforms
    uint32_t count = g_renderer.object_count();
    for (uint32_t i = 0; i < count; ++i) {
        float angle = g_elapsed * 0.5f + static_cast<float>(i) * 0.1f;
        float4x4 t = float4x4::identity();
        // Keep original grid position, add gentle bobbing
        float bob = std::sin(angle) * 0.2f;
        // Rotate instance slowly
        float c = std::cos(angle * 0.3f), s = std::sin(angle * 0.3f);
        t.m[0][0] = c;  t.m[0][2] = s;
        t.m[2][0] = -s; t.m[2][2] = c;

        // Grid layout
        int row = static_cast<int>(i) / 10;
        int col = static_cast<int>(i) % 10;
        t.set_translation(
            (static_cast<float>(col) - 4.5f) * 1.2f,
            bob,
            (static_cast<float>(row) - 4.5f) * 1.2f
        );

        g_renderer.update_transform(i, t);
    }

    // Render with stored camera matrices
    float view[16], proj[16];
    std::memcpy(view, &eye, 0); // use set_camera / set_perspective internally
    // The renderer uses its internal view/projection
    g_renderer.render(nullptr, nullptr);  // uses internal matrices

    g_renderer.end_frame();
}

// ---- Resize callback ----

static EM_BOOL on_resize(int /*event_type*/, const EmscriptenUiEvent* /*event*/, void* /*user_data*/) {
    double css_w, css_h;
    emscripten_get_element_css_size("#canvas", &css_w, &css_h);
    float dpr = static_cast<float>(emscripten_get_device_pixel_ratio());
    uint32_t w = static_cast<uint32_t>(css_w * dpr);
    uint32_t h = static_cast<uint32_t>(css_h * dpr);
    emscripten_set_canvas_element_size("#canvas", w, h);
    g_renderer.resize(w, h);
    return EM_TRUE;
}

// ---- Entry point ----

int main() {
    std::printf("Pictor WebGL2 Demo\n");

    WebGLRendererConfig config;
    config.context_config.canvas_selector = "#canvas";
    config.max_objects = 10000;
    config.enable_culling = true;

    if (!g_renderer.initialize(config)) {
        std::fprintf(stderr, "Failed to initialize WebGL renderer\n");
        return 1;
    }

    // Register a 10x10 grid of objects with varied colors
    for (int row = 0; row < 10; ++row) {
        for (int col = 0; col < 10; ++col) {
            ObjectDescriptor desc;
            desc.mesh     = 0;
            desc.material = 0;
            desc.flags    = ObjectFlags::DYNAMIC;

            float4x4 t = float4x4::identity();
            t.set_translation(
                (static_cast<float>(col) - 4.5f) * 1.2f,
                0.0f,
                (static_cast<float>(row) - 4.5f) * 1.2f
            );
            desc.transform = t;

            ObjectId id = g_renderer.register_object(desc);

            // HSV-like color ramp
            float hue = static_cast<float>(row * 10 + col) / 100.0f;
            float r = std::abs(hue * 6.0f - 3.0f) - 1.0f;
            float g = 2.0f - std::abs(hue * 6.0f - 2.0f);
            float b = 2.0f - std::abs(hue * 6.0f - 4.0f);
            r = std::fmax(0.0f, std::fmin(1.0f, r));
            g = std::fmax(0.0f, std::fmin(1.0f, g));
            b = std::fmax(0.0f, std::fmin(1.0f, b));
            g_renderer.update_color(id, r * 0.8f + 0.2f, g * 0.8f + 0.2f, b * 0.8f + 0.2f);
        }
    }

    // Set up resize handler
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_FALSE, on_resize);

    g_last_time = emscripten_get_now() / 1000.0;

    // Start animation loop
    emscripten_set_main_loop(main_loop, 0, EM_FALSE);

    std::printf("Pictor WebGL2 Demo: %u objects registered\n", g_renderer.object_count());
    return 0;
}

#else
#include <cstdio>
int main() {
    std::fprintf(stderr, "This demo requires PICTOR_HAS_WEBGL (Emscripten build)\n");
    return 1;
}
#endif
