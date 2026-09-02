#version 450

// Rope tube (world-space static geometry). Set 0 binding 0 = SceneUBO.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 cameraPos;
};

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragUV;

void main() {
    fragWorldPos = inPosition;
    fragNormal   = inNormal;
    fragUV       = inUV;
    gl_Position  = proj * view * vec4(inPosition, 1.0);
}
