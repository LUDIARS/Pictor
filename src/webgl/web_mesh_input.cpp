// Validation and vertex packing for the Web mesh module. Pure CPU code (native-testable).

#include "pictor/webgl/web_mesh_input.h"

#include <cmath>
#include <limits>

namespace pictor {

namespace {

bool all_finite(const float* values, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (!std::isfinite(values[i])) return false;
    }
    return true;
}

bool in_unit_range(float value) {
    return value >= 0.0f && value <= 1.0f;
}

bool unit_rgb(const float (&rgb)[3]) {
    return in_unit_range(rgb[0]) && in_unit_range(rgb[1]) && in_unit_range(rgb[2]);
}

bool finite_rgb(const float (&rgb)[3]) {
    return std::isfinite(rgb[0]) && std::isfinite(rgb[1]) && std::isfinite(rgb[2]);
}

} // namespace

const char* web_mesh_input_error_name(WebMeshInputError error) {
    switch (error) {
        case WebMeshInputError::NONE:               return "none";
        case WebMeshInputError::MISSING_ARRAY:      return "missing_array";
        case WebMeshInputError::EMPTY:              return "empty";
        case WebMeshInputError::NOT_TRIANGLES:      return "not_triangles";
        case WebMeshInputError::INDEX_OUT_OF_RANGE: return "index_out_of_range";
        case WebMeshInputError::NON_FINITE_VALUE:   return "non_finite_value";
        case WebMeshInputError::COLOR_OUT_OF_RANGE: return "color_out_of_range";
        case WebMeshInputError::TOO_LARGE:          return "too_large";
    }
    return "unknown";
}

WebMeshInputError validate_web_mesh_input(const WebMeshInput& input) {
    if (!input.positions || !input.normals || !input.colors || !input.indices) {
        return WebMeshInputError::MISSING_ARRAY;
    }
    if (input.vertex_count == 0 || input.index_count == 0) return WebMeshInputError::EMPTY;
    if (input.index_count % 3 != 0) return WebMeshInputError::NOT_TRIANGLES;
    if (input.vertex_count > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
        return WebMeshInputError::TOO_LARGE;
    }
    const std::size_t components = input.vertex_count * 3;
    if (!all_finite(input.positions, components) || !all_finite(input.normals, components) ||
        !all_finite(input.colors, components)) {
        return WebMeshInputError::NON_FINITE_VALUE;
    }
    for (std::size_t i = 0; i < components; ++i) {
        if (!in_unit_range(input.colors[i])) return WebMeshInputError::COLOR_OUT_OF_RANGE;
    }
    for (std::size_t i = 0; i < input.index_count; ++i) {
        if (input.indices[i] >= input.vertex_count) return WebMeshInputError::INDEX_OUT_OF_RANGE;
    }
    return WebMeshInputError::NONE;
}

float srgb_to_linear(float value) {
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

void pack_web_mesh_vertices(const WebMeshInput& input, WebColorSpace color_space,
                            std::vector<float>& out) {
    out.resize(input.vertex_count * WEB_MESH_VERTEX_FLOATS);
    for (std::size_t v = 0; v < input.vertex_count; ++v) {
        float* dst = out.data() + v * WEB_MESH_VERTEX_FLOATS;
        const std::size_t src = v * 3;
        for (std::size_t c = 0; c < 3; ++c) {
            dst[c]     = input.positions[src + c];
            dst[3 + c] = input.normals[src + c];
            const float color = input.colors[src + c];
            dst[6 + c] = color_space == WebColorSpace::SRGB ? srgb_to_linear(color) : color;
        }
    }
}

const char* web_lighting_error_name(WebLightingError error) {
    switch (error) {
        case WebLightingError::NONE:             return "none";
        case WebLightingError::TOO_MANY_LIGHTS:  return "too_many_lights";
        case WebLightingError::ZERO_DIRECTION:   return "zero_direction";
        case WebLightingError::NON_FINITE_VALUE: return "non_finite_value";
        case WebLightingError::OUT_OF_RANGE:     return "out_of_range";
    }
    return "unknown";
}

WebLightingError validate_web_lighting(const WebLighting& lighting) {
    if (lighting.directional_count > WEB_MAX_DIRECTIONAL_LIGHTS) return WebLightingError::TOO_MANY_LIGHTS;
    if (!finite_rgb(lighting.sky_color) || !finite_rgb(lighting.ground_color) ||
        !std::isfinite(lighting.hemisphere_intensity)) {
        return WebLightingError::NON_FINITE_VALUE;
    }
    if (!unit_rgb(lighting.sky_color) || !unit_rgb(lighting.ground_color) ||
        lighting.hemisphere_intensity < 0.0f) {
        return WebLightingError::OUT_OF_RANGE;
    }
    for (uint32_t i = 0; i < lighting.directional_count; ++i) {
        const WebDirectionalLight& light = lighting.directional[i];
        if (!finite_rgb(light.direction) || !finite_rgb(light.color) || !std::isfinite(light.intensity)) {
            return WebLightingError::NON_FINITE_VALUE;
        }
        const float length_sq = light.direction[0] * light.direction[0] +
                                light.direction[1] * light.direction[1] +
                                light.direction[2] * light.direction[2];
        if (!(length_sq > 0.0f)) return WebLightingError::ZERO_DIRECTION;
        if (!unit_rgb(light.color) || light.intensity < 0.0f) return WebLightingError::OUT_OF_RANGE;
    }
    return WebLightingError::NONE;
}

WebLightingError validate_web_surface(const WebSurface& surface) {
    if (!std::isfinite(surface.roughness) || !std::isfinite(surface.metalness)) {
        return WebLightingError::NON_FINITE_VALUE;
    }
    if (!in_unit_range(surface.roughness) || !in_unit_range(surface.metalness)) {
        return WebLightingError::OUT_OF_RANGE;
    }
    return WebLightingError::NONE;
}

} // namespace pictor
