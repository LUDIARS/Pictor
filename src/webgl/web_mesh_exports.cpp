// C ABI of the Web mesh module for JavaScript hosts (Emscripten exports).
// Every function returns 0 / false on failure; pictor_web_last_error() then names the cause.
// Renderer ids index a table so one page can own several canvases.

#if defined(PICTOR_HAS_WEBGL) && defined(__EMSCRIPTEN__)

#include "pictor/webgl/web_mesh_renderer.h"

#include <emscripten/emscripten.h>
#include <memory>
#include <string>
#include <vector>

using namespace pictor;

namespace {

std::vector<std::unique_ptr<WebMeshRenderer>> g_renderers;  // index = id - 1
std::string g_last_error;

int report(const std::string& message) {
    g_last_error = message;
    return 0;
}

WebMeshRenderer* find_renderer(uint32_t id) {
    if (id == 0 || id > g_renderers.size() || !g_renderers[id - 1]) {
        g_last_error = "unknown renderer";
        return nullptr;
    }
    return g_renderers[id - 1].get();
}

int from_renderer(WebMeshRenderer& renderer, bool ok) {
    return ok ? 1 : report(renderer.last_error());
}

} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE const char* pictor_web_last_error() {
    return g_last_error.c_str();
}

EMSCRIPTEN_KEEPALIVE uint32_t pictor_web_renderer_create(const char* canvas_selector, int antialias) {
    auto renderer = std::make_unique<WebMeshRenderer>();
    WebMeshRendererConfig config;
    config.canvas_selector = canvas_selector;
    config.antialias = antialias != 0;
    if (!renderer->initialize(config)) return static_cast<uint32_t>(report(renderer->last_error()));
    g_renderers.push_back(std::move(renderer));
    return static_cast<uint32_t>(g_renderers.size());
}

EMSCRIPTEN_KEEPALIVE int pictor_web_renderer_destroy(uint32_t id) {
    WebMeshRenderer* renderer = find_renderer(id);
    if (!renderer) return 0;
    renderer->shutdown();
    g_renderers[id - 1].reset();
    return 1;
}

EMSCRIPTEN_KEEPALIVE uint32_t pictor_web_mesh_create(uint32_t id, const float* positions, const float* normals,
                                                     const float* colors, uint32_t vertex_count,
                                                     const uint32_t* indices, uint32_t index_count,
                                                     int colors_are_srgb) {
    WebMeshRenderer* renderer = find_renderer(id);
    if (!renderer) return 0;
    WebMeshInput input;
    input.positions = positions;
    input.normals = normals;
    input.colors = colors;
    input.vertex_count = vertex_count;
    input.indices = indices;
    input.index_count = index_count;
    const WebMeshHandle handle =
        renderer->create_mesh(input, colors_are_srgb ? WebColorSpace::SRGB : WebColorSpace::LINEAR);
    if (handle == INVALID_WEB_MESH) return static_cast<uint32_t>(report(renderer->last_error()));
    return handle;
}

EMSCRIPTEN_KEEPALIVE int pictor_web_mesh_release(uint32_t id, uint32_t mesh) {
    WebMeshRenderer* renderer = find_renderer(id);
    return renderer ? from_renderer(*renderer, renderer->release_mesh(mesh)) : 0;
}

/// values: sky rgb, ground rgb, hemisphere intensity, light count,
///         then per light: direction xyz, colour rgb, intensity (7 floats) x WEB_MAX_DIRECTIONAL_LIGHTS.
EMSCRIPTEN_KEEPALIVE int pictor_web_set_lighting(uint32_t id, const float* values) {
    WebMeshRenderer* renderer = find_renderer(id);
    if (!renderer) return 0;
    if (!values) return report("lighting values are missing");
    WebLighting lighting;
    for (int c = 0; c < 3; ++c) {
        lighting.sky_color[c] = values[c];
        lighting.ground_color[c] = values[3 + c];
    }
    lighting.hemisphere_intensity = values[6];
    if (!(values[7] >= 0.0f) || values[7] > static_cast<float>(WEB_MAX_DIRECTIONAL_LIGHTS)) {
        return report("invalid lighting: too_many_lights");
    }
    lighting.directional_count = static_cast<uint32_t>(values[7]);
    for (uint32_t i = 0; i < lighting.directional_count; ++i) {
        const float* light = values + 8 + i * 7;
        for (int c = 0; c < 3; ++c) {
            lighting.directional[i].direction[c] = light[c];
            lighting.directional[i].color[c] = light[3 + c];
        }
        lighting.directional[i].intensity = light[6];
    }
    return from_renderer(*renderer, renderer->set_lighting(lighting));
}

EMSCRIPTEN_KEEPALIVE int pictor_web_set_surface(uint32_t id, float roughness, float metalness) {
    WebMeshRenderer* renderer = find_renderer(id);
    if (!renderer) return 0;
    WebSurface surface;
    surface.roughness = roughness;
    surface.metalness = metalness;
    return from_renderer(*renderer, renderer->set_surface(surface));
}

EMSCRIPTEN_KEEPALIVE int pictor_web_begin_frame(uint32_t id, uint32_t width, uint32_t height,
                                                const float* view_projection, const float* camera_position,
                                                float clear_r, float clear_g, float clear_b) {
    WebMeshRenderer* renderer = find_renderer(id);
    if (!renderer) return 0;
    WebFrame frame;
    frame.width = width;
    frame.height = height;
    frame.view_projection = view_projection;
    frame.camera_position = camera_position;
    frame.clear_color[0] = clear_r;
    frame.clear_color[1] = clear_g;
    frame.clear_color[2] = clear_b;
    return from_renderer(*renderer, renderer->begin_frame(frame));
}

EMSCRIPTEN_KEEPALIVE int pictor_web_submit(uint32_t id, uint32_t mesh, const float* model) {
    WebMeshRenderer* renderer = find_renderer(id);
    return renderer ? from_renderer(*renderer, renderer->submit(mesh, model)) : 0;
}

EMSCRIPTEN_KEEPALIVE int pictor_web_end_frame(uint32_t id) {
    WebMeshRenderer* renderer = find_renderer(id);
    return renderer ? from_renderer(*renderer, renderer->end_frame()) : 0;
}

} // extern "C"

#endif // PICTOR_HAS_WEBGL && __EMSCRIPTEN__
