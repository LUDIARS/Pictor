#version 300 es
precision highp float;

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;

uniform mat4 u_model;
uniform mat4 u_view_projection;

out vec3 v_world_normal;
out vec3 v_world_pos;

void main() {
    vec4 world_pos = u_model * vec4(a_position, 1.0);
    gl_Position    = u_view_projection * world_pos;
    v_world_normal = mat3(u_model) * a_normal;
    v_world_pos    = world_pos.xyz;
}
