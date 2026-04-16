#version 450

// ============================================================
// Pictor General-Purpose Lit Vertex Shader (with skinning)
//
// Vertex layout (56 bytes):
//   location 0: vec3  position
//   location 1: vec3  normal
//   location 2: uvec4 joint_indices   (up to 4 bone influences)
//   location 3: vec4  joint_weights   (sum should be 1.0)
//
// For non-skinned meshes, set joint_indices = (0,0,0,0) and
// joint_weights = (1,0,0,0), and upload an identity bone matrix
// at bone_matrices[0].
//
// Set 0:
//   binding 0 (UBO):  SceneUBO  — view, proj, light, camera
//   binding 1 (SSBO): instances[] — per-instance model matrix + color
//   binding 2 (SSBO): bone_matrices[] — skinning transforms
// ============================================================

layout(location = 0) in vec3  inPosition;
layout(location = 1) in vec3  inNormal;
layout(location = 2) in uvec4 inJoints;
layout(location = 3) in vec4  inWeights;

layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;     // xyz = world-space direction toward light, w = intensity
    vec4 lightColor;   // rgb, a = ambient intensity
    vec4 cameraPos;    // xyz = world-space camera position
};

struct InstanceData {
    mat4 model;
    vec4 baseColor;    // rgb = Lambert diffuse albedo, a = opacity
    // xy = (bone_offset, bone_count) so the same SSBO can be shared across
    // multiple skinned instances. z, w reserved.
    uvec4 skinInfo;
};
layout(std430, set = 0, binding = 1) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

layout(std430, set = 0, binding = 2) readonly buffer BoneMatricesBuffer {
    mat4 bone_matrices[];
};

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out flat uint fragInstanceID;

void main() {
    InstanceData inst = instances[gl_InstanceIndex];

    // ── Skinning (linear blend) ──────────────────────────
    uint boneOffset = inst.skinInfo.x;
    float wSum = inWeights.x + inWeights.y + inWeights.z + inWeights.w;

    mat4 skinMat;
    if (wSum > 0.0001) {
        skinMat  = bone_matrices[boneOffset + inJoints.x] * inWeights.x;
        skinMat += bone_matrices[boneOffset + inJoints.y] * inWeights.y;
        skinMat += bone_matrices[boneOffset + inJoints.z] * inWeights.z;
        skinMat += bone_matrices[boneOffset + inJoints.w] * inWeights.w;
    } else {
        // Fall back to identity so unskinned meshes still render.
        skinMat = mat4(1.0);
    }

    vec4 skinnedPos    = skinMat * vec4(inPosition, 1.0);
    vec3 skinnedNormal = mat3(skinMat) * inNormal;

    // ── Model / view / projection ────────────────────────
    vec4 worldPos = inst.model * skinnedPos;
    mat3 normalMat = mat3(inst.model);
    fragWorldPos   = worldPos.xyz;
    fragNormal     = normalize(normalMat * skinnedNormal);
    fragInstanceID = uint(gl_InstanceIndex);
    gl_Position    = proj * view * worldPos;
}
