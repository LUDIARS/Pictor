#version 450

// ============================================================
// Skinned + textured vertex shader for the FBX Viewer.
//
// Layout (64 bytes):
//   location 0: vec3  position
//   location 1: vec3  normal
//   location 2: vec2  uv
//   location 3: uvec4 joint_indices
//   location 4: vec4  joint_weights
//
// Set 0:
//   binding 0: SceneUBO   — view, proj, light, camera
//   binding 1: instances  — per-instance model matrix + color
//   binding 2: bones      — skinning matrices
// Set 1:
//   binding 0: base color sampler2D (re-bound per submesh)
// ============================================================

layout(location = 0) in vec3  inPosition;
layout(location = 1) in vec3  inNormal;
layout(location = 2) in vec2  inUV;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4  inWeights;

layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;    // xyz = direction toward light, w = intensity
    vec4 lightColor;  // rgb + ambient
    vec4 cameraPos;
};

struct InstanceData {
    mat4  model;
    vec4  baseColor;
    uvec4 skinInfo;   // x = bone_offset, y = bone_count
};
layout(std430, set = 0, binding = 1) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

layout(std430, set = 0, binding = 2) readonly buffer BoneMatricesBuffer {
    mat4 bone_matrices[];
};

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragUV;
layout(location = 3) out flat uint fragInstanceID;

void main() {
    InstanceData inst = instances[gl_InstanceIndex];

    uint boneOffset = inst.skinInfo.x;
    float wSum = inWeights.x + inWeights.y + inWeights.z + inWeights.w;

    mat4 skinMat;
    if (wSum > 0.0001) {
        skinMat  = bone_matrices[boneOffset + inJoints.x] * inWeights.x;
        skinMat += bone_matrices[boneOffset + inJoints.y] * inWeights.y;
        skinMat += bone_matrices[boneOffset + inJoints.z] * inWeights.z;
        skinMat += bone_matrices[boneOffset + inJoints.w] * inWeights.w;
    } else {
        skinMat = mat4(1.0);
    }

    vec4 skinnedPos    = skinMat * vec4(inPosition, 1.0);
    vec3 skinnedNormal = mat3(skinMat) * inNormal;

    vec4 worldPos = inst.model * skinnedPos;
    fragWorldPos   = worldPos.xyz;
    fragNormal     = normalize(mat3(inst.model) * skinnedNormal);
    fragUV         = inUV;
    fragInstanceID = uint(gl_InstanceIndex);
    gl_Position    = proj * view * worldPos;
}
