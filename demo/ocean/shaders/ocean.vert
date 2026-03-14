// Pictor — Ocean Wave Vertex Shader
// Passes grid patch vertices to tessellation control shader.

#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord;

layout(location = 0) out vec3 vsPosition;
layout(location = 1) out vec2 vsTexCoord;

void main() {
    vsPosition = inPosition;
    vsTexCoord = inTexCoord;
}
