#version 450

// Pictor — Rive Cube vertex shader.
// 6-face cube: each face samples its own 2048x2048 Rive output texture.
// The push-constant MVP is stored row-major by the host (see the Mat4
// helpers in demo/rive_cube/main.cpp); declare the qualifier here so
// GLSL's default column-major reading does not transpose the matrix.

layout(row_major, push_constant) uniform PushConstant {
    mat4 mvp;
} pc;

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;

layout(location = 0) out vec2 v_uv;

void main() {
    v_uv = a_uv;
    gl_Position = pc.mvp * vec4(a_pos, 1.0);
}
