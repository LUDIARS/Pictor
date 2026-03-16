#version 300 es
precision highp float;

// Per-vertex attributes
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;

// Per-instance attributes (mat4 = 4 x vec4)
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

    // Normal transform (upper-left 3x3 — assumes uniform scale)
    v_world_normal = mat3(model) * a_normal;
    v_world_pos    = world_pos.xyz;
    v_color        = a_color;
}
