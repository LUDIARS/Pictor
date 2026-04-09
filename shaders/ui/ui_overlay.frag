#version 450

// UI Overlay fragment shader — texture * per-vertex color

layout(set = 0, binding = 0) uniform sampler2D u_texture;

layout(location = 0) in vec2 v_texCoord;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 fragColor;

void main() {
    vec4 texel  = texture(u_texture, v_texCoord);
    fragColor   = texel * v_color;
}
