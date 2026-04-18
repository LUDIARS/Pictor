#version 450

// Debug bone visualisation — draws one line per bone from child to parent.
// Shares the scene UBO with the mesh pipeline (set 0 binding 0).

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;

layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 cameraPos;
};

layout(location = 0) out vec3 fragColor;

void main() {
    fragColor = inColor;
    gl_Position = proj * view * vec4(inPosition, 1.0);
}
