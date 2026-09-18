#version 450
#extension GL_EXT_multiview : require

// multiview: 1 回の描画が 2 層へ走り、 gl_ViewIndex が眼を表す。
layout(push_constant) uniform Push { mat4 view_proj[2]; } pc;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in mat4 in_model;   // インスタンスごと (location 2..5)
layout(location = 6) in vec4 in_color;   // インスタンスごと

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec4 v_color;

void main() {
    // @implements SPEC-PC-XR-DEMO
    vec4 world  = in_model * vec4(in_position, 1.0);
    gl_Position = pc.view_proj[gl_ViewIndex] * world;
    v_normal    = mat3(in_model) * in_normal;
    v_color     = in_color;
}
