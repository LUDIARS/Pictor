#version 450

// Flat passthrough fragment shader shared by the node and edge pipelines.

layout(location = 0) in  vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vColor;
}
