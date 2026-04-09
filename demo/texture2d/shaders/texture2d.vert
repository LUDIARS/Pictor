#version 450

layout(push_constant) uniform PushConstants {
    mat4 projection;
    mat4 model;
    vec4 tint;     // rgb = tint, a = opacity
} pc;

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_texCoord;

layout(location = 0) out vec2 v_texCoord;

void main() {
    gl_Position = pc.projection * pc.model * vec4(a_position, 0.0, 1.0);
    v_texCoord  = a_texCoord;
}
