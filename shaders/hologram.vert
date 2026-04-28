#version 450

// ============================================================
// Pictor Hologram Vertex Shader
//
// Minimal pass-through that forwards world position + normal to the
// fragment stage. Shares the SceneUBO + InstanceBuffer layout with
// lit.{vert,frag} so a hologram-styled draw can swap shaders without
// changing descriptors. No skinning — hologram targets static SkillBox
// / Equipment cubes.
//
// Vertex layout (24 bytes):
//   location 0: vec3 position
//   location 1: vec3 normal
// ============================================================

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 cameraPos;
};

struct InstanceData {
    mat4  model;
    vec4  baseColor;
    uvec4 skinInfo;
};
layout(std430, set = 0, binding = 1) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out flat uint fragInstanceID;

void main() {
    InstanceData inst = instances[gl_InstanceIndex];
    vec4 worldPos = inst.model * vec4(inPosition, 1.0);
    fragWorldPos   = worldPos.xyz;
    fragNormal     = normalize(mat3(inst.model) * inNormal);
    fragInstanceID = uint(gl_InstanceIndex);
    gl_Position    = proj * view * worldPos;
}
