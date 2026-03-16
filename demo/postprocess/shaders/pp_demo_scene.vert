// Pictor — Post-Process Demo Scene Vertex Shader
// Transforms world positions and normals for PBR scene rendering.

#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) flat out uint fragInstanceID;

// Scene uniforms
layout(set = 0, binding = 0) uniform SceneParams {
    mat4  view;
    mat4  projection;
    mat4  viewProjection;
    vec4  cameraPosition;
    vec4  ambientColor;

    vec4  sunDirection;
    vec4  sunColor;

    float time;
    float dofFocusDistance;
    float dofFocusRange;
    float exposure;
};

// Instance data
struct InstanceData {
    mat4 model;
    vec4 baseColor;
    vec4 pbrParams;       // metallic, roughness, ao, emissiveStrength
    vec4 emissiveColor;
};

layout(std430, set = 0, binding = 1) readonly buffer Instances {
    InstanceData instances[];
};

void main() {
    InstanceData inst = instances[gl_InstanceIndex];

    vec4 worldPos = inst.model * vec4(inPosition, 1.0);
    fragWorldPos = worldPos.xyz;
    fragNormal   = normalize(mat3(inst.model) * inNormal);
    fragInstanceID = gl_InstanceIndex;

    gl_Position = viewProjection * worldPos;
}
