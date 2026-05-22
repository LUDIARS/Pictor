#version 450

// Pictor — Rive Cube fragment shader.
// Samples the bound Rive-rendered face texture with linear + anisotropic
// filtering (configured on the sampler in main.cpp).

layout(set = 0, binding = 0) uniform sampler2D u_face;

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = texture(u_face, v_uv);
}
