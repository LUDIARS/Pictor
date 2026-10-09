#pragma once

// GLSL ES 3.00 sources of the Web mesh module. Shading matches a metal/rough material:
// Lambert diffuse (albedo / pi), GGX specular for directional lights, hemisphere ambient,
// output encoded to sRGB (no tone mapping).

namespace pictor::web_mesh_shaders {

inline constexpr const char* VERTEX = R"GLSL(#version 300 es
precision highp float;
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec3 a_color;
uniform mat4 u_model;
uniform mat4 u_view_projection;
out vec3 v_world_position;
out vec3 v_normal;
out vec3 v_color;
void main() {
    vec4 world = u_model * vec4(a_position, 1.0);
    v_world_position = world.xyz;
    // Uniform scale assumed: the upper 3x3 of the model matrix rotates normals.
    v_normal = mat3(u_model) * a_normal;
    v_color = a_color;
    gl_Position = u_view_projection * world;
}
)GLSL";

inline constexpr const char* FRAGMENT = R"GLSL(#version 300 es
precision highp float;
const float PI = 3.141592653589793;
const int MAX_LIGHTS = 2;
in vec3 v_world_position;
in vec3 v_normal;
in vec3 v_color;
uniform vec3 u_camera_position;
uniform vec3 u_sky;          // linear, premultiplied by intensity
uniform vec3 u_ground;       // linear, premultiplied by intensity
uniform int u_light_count;
uniform vec3 u_light_direction[MAX_LIGHTS];  // normalized, towards the light
uniform vec3 u_light_radiance[MAX_LIGHTS];   // linear colour * intensity
uniform float u_roughness;
uniform float u_metalness;
out vec4 frag_color;

vec3 fresnel_schlick(vec3 f0, float cos_theta) {
    float fresnel = pow(1.0 - cos_theta, 5.0);
    return f0 + (1.0 - f0) * fresnel;
}

float visibility_smith_ggx(float alpha, float n_dot_l, float n_dot_v) {
    float a2 = alpha * alpha;
    float gv = n_dot_l * sqrt(a2 + (1.0 - a2) * n_dot_v * n_dot_v);
    float gl = n_dot_v * sqrt(a2 + (1.0 - a2) * n_dot_l * n_dot_l);
    return 0.5 / max(gv + gl, 1e-6);
}

float distribution_ggx(float alpha, float n_dot_h) {
    float a2 = alpha * alpha;
    float denom = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

vec3 linear_to_srgb(vec3 c) {
    vec3 low = c * 12.92;
    vec3 high = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(low, high, step(vec3(0.0031308), c));
}

void main() {
    vec3 n = normalize(v_normal);
    vec3 v = normalize(u_camera_position - v_world_position);
    vec3 albedo = v_color;
    vec3 diffuse_color = albedo * (1.0 - u_metalness);
    vec3 f0 = mix(vec3(0.04), albedo, u_metalness);
    float alpha = max(u_roughness * u_roughness, 1e-3);
    float n_dot_v = clamp(dot(n, v), 1e-4, 1.0);

    vec3 hemisphere = mix(u_ground, u_sky, 0.5 * n.y + 0.5);
    vec3 color = diffuse_color / PI * hemisphere;

    for (int i = 0; i < MAX_LIGHTS; ++i) {
        if (i >= u_light_count) break;
        vec3 l = u_light_direction[i];
        float n_dot_l = clamp(dot(n, l), 0.0, 1.0);
        if (n_dot_l <= 0.0) continue;
        vec3 h = normalize(l + v);
        float n_dot_h = clamp(dot(n, h), 0.0, 1.0);
        float v_dot_h = clamp(dot(v, h), 0.0, 1.0);
        vec3 specular = fresnel_schlick(f0, v_dot_h)
            * visibility_smith_ggx(alpha, n_dot_l, n_dot_v)
            * distribution_ggx(alpha, n_dot_h);
        color += u_light_radiance[i] * n_dot_l * (diffuse_color / PI + specular);
    }
    frag_color = vec4(linear_to_srgb(clamp(color, 0.0, 1.0)), 1.0);
}
)GLSL";

} // namespace pictor::web_mesh_shaders
