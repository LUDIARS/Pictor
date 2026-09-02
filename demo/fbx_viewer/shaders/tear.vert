#version 450

// Toon tear billboard: positions are already world-space quad corners.

layout(location = 0) in vec3  inPosition;
layout(location = 1) in vec2  inCorner;
layout(location = 2) in float inAge;

layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 cameraPos;
};

layout(location = 0) out vec2  fragCorner;
layout(location = 1) out float fragAge;

void main() {
    fragCorner  = inCorner;
    fragAge     = inAge;
    gl_Position = proj * view * vec4(inPosition, 1.0);
}
