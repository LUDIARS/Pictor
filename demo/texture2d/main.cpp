/// Pictor 2D Texture Rendering Demo
///
/// Loads sample1-5.png textures and renders them as 2D quads.
/// Controls:
///   1-5    : Switch texture
///   +/-    : Scale up/down
///   Q/E    : Rotate left/right
///   A/D    : Decrease/increase opacity
///   F      : Toggle filter (Linear / Nearest)
///   T      : Cycle tint preset
///   R      : Reset transform
///   Drag   : Pan texture
///   Scroll : Zoom
///   S      : Toggle stats overlay

#include "pictor/pictor.h"
#include "pictor/surface/vulkan_context.h"
#include "pictor/surface/glfw_surface_provider.h"
#include "pictor/profiler/stats_overlay.h"
#include "pictor/profiler/bitmap_text_renderer.h"
#include "texture2d_renderer.h"

#include <GLFW/glfw3.h>
#include <stb_image.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <array>

using namespace pictor;

// ─── Texture file definitions ────────────────────────────────

static constexpr int NUM_TEXTURES = 5;
static const char* TEXTURE_FILES[NUM_TEXTURES] = {
    "textures/sample1.png",
    "textures/sample2.png",
    "textures/sample3.png",
    "textures/sample4.png",
    "textures/sample5.png",
};

// ─── Quad vertex ─────────────────────────────────────────────

struct Vertex2D {
    float pos[2];
    float uv[2];
};

// ─── Application State ──────────────────────────────────────

struct AppState {
    int   current_texture = 0;
    int   prev_texture    = -1;    // for detecting texture switch
    float pan_x    = 0.0f;
    float pan_y    = 0.0f;
    float scale    = 1.0f;
    float rotation = 0.0f;
    float opacity  = 1.0f;
    float tint[3]  = {1.0f, 1.0f, 1.0f};
    bool  nearest_filter = false;

    bool   dragging = false;
    double last_mouse_x = 0.0;
    double last_mouse_y = 0.0;

    int tint_preset = 0;
};

static AppState g_state;

// ─── Tint presets ────────────────────────────────────────────

struct TintPreset { const char* name; float r, g, b; };

static const TintPreset TINT_PRESETS[] = {
    {"White (no tint)", 1.0f, 1.0f, 1.0f},
    {"Warm",            1.0f, 0.85f, 0.7f},
    {"Cool",            0.7f, 0.85f, 1.0f},
    {"Sepia",           0.9f, 0.75f, 0.55f},
    {"Red",             1.0f, 0.3f, 0.3f},
    {"Green",           0.3f, 1.0f, 0.3f},
    {"Blue",            0.3f, 0.3f, 1.0f},
};
static constexpr int NUM_TINT_PRESETS = sizeof(TINT_PRESETS) / sizeof(TINT_PRESETS[0]);

// ─── Matrix helpers ──────────────────────────────────────────

static void mat4_ortho(float* m, float l, float r, float b, float t, float n, float f) {
    std::memset(m, 0, 16 * sizeof(float));
    m[0]  =  2.0f / (r - l);
    m[5]  =  2.0f / (t - b);
    m[10] = -2.0f / (f - n);
    m[12] = -(r + l) / (r - l);
    m[13] = -(t + b) / (t - b);
    m[14] = -(f + n) / (f - n);
    m[15] =  1.0f;
}

static void mat4_translate_rotate_scale(float* m, float tx, float ty,
                                         float angle_deg, float sx, float sy) {
    float rad = angle_deg * 3.14159265f / 180.0f;
    float c = std::cos(rad);
    float s = std::sin(rad);

    std::memset(m, 0, 16 * sizeof(float));
    m[0]  = c * sx;   m[1]  = s * sx;
    m[4]  = -s * sy;  m[5]  = c * sy;
    m[10] = 1.0f;
    m[12] = tx;        m[13] = ty;
    m[15] = 1.0f;
}

// ─── Texture loading ─────────────────────────────────────────

struct ImageData {
    uint32_t width  = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels; // RGBA
};

/// Load a PNG/JPG/BMP file via stb_image. Returns empty ImageData on failure.
static ImageData load_image_file(const char* path) {
    ImageData img;
    int w = 0, h = 0, channels = 0;

    unsigned char* data = stbi_load(path, &w, &h, &channels, 4); // force RGBA
    if (!data) {
        fprintf(stderr, "[texture] ERROR: Failed to load '%s': %s\n",
                path, stbi_failure_reason());
        return img;
    }

    img.width  = static_cast<uint32_t>(w);
    img.height = static_cast<uint32_t>(h);
    size_t byte_size = static_cast<size_t>(w) * h * 4;
    img.pixels.assign(data, data + byte_size);
    stbi_image_free(data);

    printf("[texture] Loaded '%s': %ux%u (%d ch -> RGBA, %zu bytes)\n",
           path, img.width, img.height, channels, byte_size);
    return img;
}

/// Generate a procedural checkerboard as fallback when file loading fails.
static ImageData generate_fallback_texture(const char* name, int index) {
    ImageData img;
    img.width  = 256;
    img.height = 256;
    img.pixels.resize(img.width * img.height * 4);

    uint8_t base_r = 0, base_g = 0, base_b = 0;
    switch (index % 5) {
        case 0: base_r = 200; base_g = 80;  base_b = 80;  break;
        case 1: base_r = 80;  base_g = 200; base_b = 80;  break;
        case 2: base_r = 80;  base_g = 80;  base_b = 200; break;
        case 3: base_r = 200; base_g = 200; base_b = 80;  break;
        case 4: base_r = 200; base_g = 80;  base_b = 200; break;
    }

    for (uint32_t y = 0; y < img.height; ++y) {
        for (uint32_t x = 0; x < img.width; ++x) {
            size_t idx = (y * img.width + x) * 4;
            bool checker = ((x / 32) + (y / 32)) % 2 == 0;
            float intensity = checker ? 1.0f : 0.33f;
            float gx = static_cast<float>(x) / static_cast<float>(img.width);
            float gy = static_cast<float>(y) / static_cast<float>(img.height);
            float grad = 0.6f + 0.4f * gx * gy;

            img.pixels[idx + 0] = static_cast<uint8_t>(base_r * intensity * grad);
            img.pixels[idx + 1] = static_cast<uint8_t>(base_g * intensity * grad);
            img.pixels[idx + 2] = static_cast<uint8_t>(base_b * intensity * grad);
            img.pixels[idx + 3] = 255;
        }
    }

    fprintf(stderr, "[texture] WARN: Using fallback checkerboard for '%s' (%ux%u)\n",
            name, img.width, img.height);
    return img;
}

/// Load texture from file, falling back to procedural if file not found.
static ImageData load_texture(const char* path, int index) {
    ImageData img = load_image_file(path);
    if (img.pixels.empty()) {
        img = generate_fallback_texture(path, index);
    }
    return img;
}

// ─── GLFW Callbacks ──────────────────────────────────────────

static void key_callback(GLFWwindow*, int key, int, int action, int) {
    if (action != GLFW_PRESS) return;

    if (key >= GLFW_KEY_1 && key <= GLFW_KEY_5) {
        g_state.current_texture = key - GLFW_KEY_1;
        printf("[input] Switched to sample%d\n", g_state.current_texture + 1);
    }
    if (key == GLFW_KEY_EQUAL || key == GLFW_KEY_KP_ADD)
        g_state.scale = std::min(8.0f, g_state.scale * 1.25f);
    if (key == GLFW_KEY_MINUS || key == GLFW_KEY_KP_SUBTRACT)
        g_state.scale = std::max(0.1f, g_state.scale * 0.8f);
    if (key == GLFW_KEY_R) {
        g_state.pan_x = g_state.pan_y = 0.0f;
        g_state.scale = 1.0f;
        g_state.rotation = 0.0f;
        g_state.opacity = 1.0f;
        g_state.tint_preset = 0;
        g_state.tint[0] = g_state.tint[1] = g_state.tint[2] = 1.0f;
        printf("[input] Reset\n");
    }
    if (key == GLFW_KEY_F) {
        g_state.nearest_filter = !g_state.nearest_filter;
        printf("[input] Filter: %s\n", g_state.nearest_filter ? "Nearest" : "Linear");
    }
    if (key == GLFW_KEY_T) {
        g_state.tint_preset = (g_state.tint_preset + 1) % NUM_TINT_PRESETS;
        auto& tp = TINT_PRESETS[g_state.tint_preset];
        g_state.tint[0] = tp.r; g_state.tint[1] = tp.g; g_state.tint[2] = tp.b;
        printf("[input] Tint: %s\n", tp.name);
    }
    if (key == GLFW_KEY_Q) g_state.rotation -= 15.0f;
    if (key == GLFW_KEY_E) g_state.rotation += 15.0f;
    if (key == GLFW_KEY_A) g_state.opacity = std::max(0.0f, g_state.opacity - 0.1f);
    if (key == GLFW_KEY_D) g_state.opacity = std::min(1.0f, g_state.opacity + 0.1f);
}

static void mouse_button_callback(GLFWwindow* win, int button, int action, int) {
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        g_state.dragging = (action == GLFW_PRESS);
        if (g_state.dragging)
            glfwGetCursorPos(win, &g_state.last_mouse_x, &g_state.last_mouse_y);
    }
}

static void cursor_pos_callback(GLFWwindow*, double xpos, double ypos) {
    if (!g_state.dragging) return;
    g_state.pan_x += static_cast<float>(xpos - g_state.last_mouse_x);
    g_state.pan_y += static_cast<float>(ypos - g_state.last_mouse_y);
    g_state.last_mouse_x = xpos;
    g_state.last_mouse_y = ypos;
}

static void scroll_callback(GLFWwindow*, double, double yoffset) {
    float factor = (yoffset > 0) ? 1.15f : 0.87f;
    g_state.scale = std::clamp(g_state.scale * factor, 0.05f, 16.0f);
}

// ─── Main ────────────────────────────────────────────────────

int main() {
    printf("=== Pictor 2D Texture Rendering Demo ===\n\n");

    // ── 1. Window ────────────────────────────────────────────
    printf("[init] Creating GLFW window...\n");
    GlfwSurfaceProvider surface_provider;
    GlfwWindowConfig win_cfg;
    win_cfg.width  = 1280;
    win_cfg.height = 720;
    win_cfg.title  = "Pictor — 2D Texture Demo";
    win_cfg.vsync  = true;

    if (!surface_provider.create(win_cfg)) {
        fprintf(stderr, "[init] FATAL: Failed to create GLFW window\n");
        return 1;
    }
    printf("[init] GLFW window created: %ux%u\n", win_cfg.width, win_cfg.height);

    // ── 2. Vulkan Context ────────────────────────────────────
    printf("[init] Initializing Vulkan context...\n");
    VulkanContext vk_ctx;
    VulkanContextConfig vk_cfg;
    vk_cfg.app_name   = "Pictor Texture2D Demo";
    vk_cfg.validation = true;

    if (!vk_ctx.initialize(&surface_provider, vk_cfg)) {
        fprintf(stderr, "[init] FATAL: Failed to initialize Vulkan context\n");
        surface_provider.destroy();
        return 1;
    }
    printf("[init] Vulkan context initialized. Swapchain: %ux%u\n",
           vk_ctx.swapchain_extent().width, vk_ctx.swapchain_extent().height);

    // ── 3. Pictor Renderer ───────────────────────────────────
    printf("[init] Initializing Pictor renderer...\n");
    RendererConfig pictor_cfg;
    pictor_cfg.initial_profile  = "Standard";
    pictor_cfg.screen_width     = vk_ctx.swapchain_extent().width;
    pictor_cfg.screen_height    = vk_ctx.swapchain_extent().height;
    pictor_cfg.profiler_enabled = true;
    pictor_cfg.overlay_mode     = OverlayMode::MINIMAL;

    PictorRenderer renderer;
    renderer.initialize(pictor_cfg);
    printf("[init] Pictor renderer initialized\n");

    // ── 4. Texture2D Renderer ────────────────────────────────
    printf("[init] Initializing Texture2D renderer...\n");
    Texture2DRenderer tex_renderer;
    if (!tex_renderer.initialize(vk_ctx, "shaders")) {
        fprintf(stderr, "[init] FATAL: Failed to initialize Texture2D renderer\n");
        renderer.shutdown();
        vk_ctx.shutdown();
        surface_provider.destroy();
        return 1;
    }

    // ── 5. Load Textures ─────────────────────────────────────
    printf("[init] Loading textures...\n");
    struct TextureInfo {
        ImageData   image;
        std::string name;
    };
    std::array<TextureInfo, NUM_TEXTURES> textures;

    for (int i = 0; i < NUM_TEXTURES; ++i) {
        textures[i].name = std::string("sample") + std::to_string(i + 1);
        textures[i].image = load_texture(TEXTURE_FILES[i], i);
    }
    printf("[init] All %d textures loaded\n\n", NUM_TEXTURES);

    // Upload initial texture
    {
        auto& tex = textures[0];
        printf("[init] Uploading initial texture: %s\n", tex.name.c_str());
        if (!tex_renderer.upload_texture(tex.image.pixels.data(), tex.image.width, tex.image.height)) {
            fprintf(stderr, "[init] WARNING: Failed to upload initial texture\n");
        }
        g_state.prev_texture = 0;
    }

    // ── 6. Stats Overlay ─────────────────────────────────────
#ifdef PICTOR_HAS_VULKAN
    BitmapTextRenderer text_renderer;
    if (!text_renderer.initialize(vk_ctx, "shaders")) {
        fprintf(stderr, "[init] Warning: BitmapTextRenderer init failed\n");
    }
#endif

    StatsOverlay stats_overlay;
    stats_overlay.initialize(vk_ctx.swapchain_extent().width, vk_ctx.swapchain_extent().height);
    stats_overlay.set_visible(true);
#ifdef PICTOR_HAS_VULKAN
    stats_overlay.set_text_renderer(&text_renderer);
#endif

    // ── 7. GLFW Callbacks ────────────────────────────────────
    GLFWwindow* win = surface_provider.glfw_window();

    struct CallbackData { PictorRenderer* renderer; StatsOverlay* stats; };
    static CallbackData cb_data{&renderer, &stats_overlay};
    glfwSetWindowUserPointer(win, &cb_data);

    glfwSetKeyCallback(win, [](GLFWwindow* w, int key, int scancode, int action, int mods) {
        if (key == GLFW_KEY_S && action == GLFW_PRESS) {
            auto* d = static_cast<CallbackData*>(glfwGetWindowUserPointer(w));
            d->stats->toggle();
            d->renderer->toggle_stats_overlay();
        }
        key_callback(w, key, scancode, action, mods);
    });
    glfwSetMouseButtonCallback(win, mouse_button_callback);
    glfwSetCursorPosCallback(win, cursor_pos_callback);
    glfwSetScrollCallback(win, scroll_callback);

    // ── 8. Main Loop ─────────────────────────────────────────
    uint64_t frame_count = 0;
    printf("[loop] Entering main loop...\n");
    printf("[loop] Controls: 1-5=texture, +/-=scale, Q/E=rotate, A/D=opacity, F=filter, T=tint, R=reset, S=stats\n\n");

    while (!surface_provider.should_close()) {
        surface_provider.poll_events();

        // Re-upload texture if switched
        if (g_state.current_texture != g_state.prev_texture) {
            auto& tex = textures[g_state.current_texture];
            printf("[loop] Switching texture to: %s\n", tex.name.c_str());
            vk_ctx.device_wait_idle();
            tex_renderer.upload_texture(tex.image.pixels.data(), tex.image.width, tex.image.height);
            g_state.prev_texture = g_state.current_texture;
        }

        auto ext = vk_ctx.swapchain_extent();
        float w = static_cast<float>(ext.width);
        float h = static_cast<float>(ext.height);

        auto& tex = textures[g_state.current_texture];
        float tw = static_cast<float>(tex.image.width) * g_state.scale;
        float th = static_cast<float>(tex.image.height) * g_state.scale;

        // Build push constants
        Texture2DPushConstants pc;
        mat4_ortho(pc.projection, 0.0f, w, h, 0.0f, -1.0f, 1.0f);
        mat4_translate_rotate_scale(pc.model,
            w * 0.5f + g_state.pan_x,
            h * 0.5f + g_state.pan_y,
            g_state.rotation, tw, th);
        pc.tint[0] = g_state.tint[0];
        pc.tint[1] = g_state.tint[1];
        pc.tint[2] = g_state.tint[2];
        pc.tint[3] = g_state.opacity;

        // Acquire swapchain image
        uint32_t image_idx = vk_ctx.acquire_next_image();
        if (image_idx == UINT32_MAX) {
            printf("[loop] Swapchain recreated, skipping frame\n");
            continue;
        }

#ifdef PICTOR_HAS_VULKAN
        auto cmd = vk_ctx.command_buffers()[image_idx];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &begin_info);

        VkClearValue clear_color = {{{0.12f, 0.12f, 0.18f, 1.0f}}};

        VkRenderPassBeginInfo rp_info{};
        rp_info.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_info.renderPass  = vk_ctx.default_render_pass();
        rp_info.framebuffer = vk_ctx.framebuffers()[image_idx];
        rp_info.renderArea  = {{0, 0}, ext};
        rp_info.clearValueCount = 1;
        rp_info.pClearValues    = &clear_color;

        vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

        // ★ Draw the textured quad
        tex_renderer.render(cmd, ext, pc);

        // Stats overlay
        if (stats_overlay.is_visible()) {
            stats_overlay.begin_render(cmd, ext);
            SceneSummary summary;
            summary.batch_count     = 1;
            summary.polygon_count   = 2;
            summary.draw_call_count = 1;
            stats_overlay.render(renderer.get_frame_stats(), summary);
            stats_overlay.end_render();
        }

        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphore wait_sem = vk_ctx.image_available_semaphore();
        VkSemaphore sig_sem  = vk_ctx.render_finished_semaphore();

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
#endif

        vk_ctx.present(image_idx);

        float dt = 1.0f / 60.0f;
        renderer.begin_frame(dt);
        Camera cam;
        cam.position = {0, 0, 1};
        renderer.render(cam);
        renderer.end_frame();

        ++frame_count;

        if (frame_count % 300 == 0) {
            const auto& stats = renderer.get_frame_stats();
            printf("[loop] Frame %llu | FPS: %.1f | Texture: %s (%ux%u) | Scale: %.2f | Tint: %s\n",
                   static_cast<unsigned long long>(frame_count),
                   stats.fps,
                   tex.name.c_str(),
                   tex.image.width, tex.image.height,
                   g_state.scale,
                   TINT_PRESETS[g_state.tint_preset].name);
        }
    }

    // ── 9. Cleanup ───────────────────────────────────────────
    printf("\n[cleanup] Shutting down...\n");
    vk_ctx.device_wait_idle();
    tex_renderer.shutdown();
#ifdef PICTOR_HAS_VULKAN
    text_renderer.shutdown();
#endif
    renderer.shutdown();
    vk_ctx.shutdown();
    surface_provider.destroy();

    printf("[cleanup] Done. %llu frames rendered.\n",
           static_cast<unsigned long long>(frame_count));
    return 0;
}
