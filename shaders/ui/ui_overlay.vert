#version 450

// UI Overlay vertex shader — per-vertex color, pre-transformed NDC positions

layout(location = 0) in vec2  a_position;  // NDC [-1, 1]
layout(location = 1) in vec2  a_texCoord;
layout(location = 2) in vec4  a_color;     // RGBA tint

layout(location = 0) out vec2 v_texCoord;
layout(location = 1) out vec4 v_color;

void main() {
    gl_Position = vec4(a_position, 0.0, 1.0);
    v_texCoord  = a_texCoord;
    v_color     = a_color;
}
