#pragma once

// Input contract of the Web mesh module (SPEC-PC-WEB-MESH): indexed triangle meshes with
// per-vertex position, normal and colour, plus the lighting/surface parameters of a frame.
//
// Pure CPU code — no GL, no Emscripten — so the validation and vertex packing are tested on
// native builds. The GL renderer (web_mesh_renderer.h) only consumes already validated data.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pictor {

/// One indexed triangle mesh as handed over by a host. Arrays are borrowed for the call.
struct WebMeshInput {
    const float*    positions    = nullptr;  ///< xyz per vertex
    const float*    normals      = nullptr;  ///< xyz per vertex
    const float*    colors       = nullptr;  ///< rgb per vertex, 0..1
    std::size_t     vertex_count = 0;
    const uint32_t* indices      = nullptr;  ///< triangle list
    std::size_t     index_count  = 0;
};

/// How the host wrote the vertex colours. Shading happens in linear space.
enum class WebColorSpace : uint8_t {
    SRGB   = 0,  ///< converted to linear while packing
    LINEAR = 1,
};

enum class WebMeshInputError : uint8_t {
    NONE = 0,
    MISSING_ARRAY,         ///< a required pointer is null
    EMPTY,                 ///< no vertices or no indices
    NOT_TRIANGLES,         ///< index_count is not a multiple of 3
    INDEX_OUT_OF_RANGE,    ///< an index >= vertex_count
    NON_FINITE_VALUE,      ///< NaN / Inf in positions, normals or colours
    COLOR_OUT_OF_RANGE,    ///< a colour component outside 0..1
    TOO_LARGE,             ///< more vertices than a 32-bit index can address
};

/// Human readable name of an error, stable for logs and the JS error message.
const char* web_mesh_input_error_name(WebMeshInputError error);

/// Checks the whole input. Nothing is uploaded unless this returns NONE.
WebMeshInputError validate_web_mesh_input(const WebMeshInput& input);

/// Floats per packed vertex: position (3) + normal (3) + linear colour (3).
constexpr std::size_t WEB_MESH_VERTEX_FLOATS = 9;

/// Interleaves a validated mesh into position/normal/linear-colour vertices.
/// `out` is resized to vertex_count * WEB_MESH_VERTEX_FLOATS.
void pack_web_mesh_vertices(const WebMeshInput& input, WebColorSpace color_space,
                            std::vector<float>& out);

/// sRGB transfer function (IEC 61966-2-1), component-wise, input 0..1.
float srgb_to_linear(float value);

/// Up to this many directional lights per frame.
constexpr uint32_t WEB_MAX_DIRECTIONAL_LIGHTS = 2;

struct WebDirectionalLight {
    float direction[3] = {0.0f, 1.0f, 0.0f};  ///< from the surface towards the light, any length > 0
    float color[3]     = {1.0f, 1.0f, 1.0f};  ///< sRGB 0..1
    float intensity    = 1.0f;
};

/// Hemisphere ambient (sky above, ground below along +Y) plus directional lights.
/// Intensities follow the physically based convention: diffuse = albedo / pi * irradiance.
struct WebLighting {
    float    sky_color[3]       = {1.0f, 1.0f, 1.0f};  ///< sRGB 0..1
    float    ground_color[3]    = {0.0f, 0.0f, 0.0f};  ///< sRGB 0..1
    float    hemisphere_intensity = 1.0f;
    uint32_t directional_count  = 0;
    WebDirectionalLight directional[WEB_MAX_DIRECTIONAL_LIGHTS];
};

/// Metal/rough surface shared by every mesh drawn by one renderer.
struct WebSurface {
    float roughness = 0.5f;   ///< 0..1
    float metalness = 0.0f;   ///< 0..1
};

enum class WebLightingError : uint8_t {
    NONE = 0,
    TOO_MANY_LIGHTS,
    ZERO_DIRECTION,
    NON_FINITE_VALUE,
    OUT_OF_RANGE,          ///< negative intensity, colour outside 0..1, surface outside 0..1
};

const char* web_lighting_error_name(WebLightingError error);
WebLightingError validate_web_lighting(const WebLighting& lighting);
WebLightingError validate_web_surface(const WebSurface& surface);

} // namespace pictor
