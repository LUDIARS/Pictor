#version 450
#extension GL_GOOGLE_include_directive : require

// Shell-fur vertex shader: same skinning as model.vert, then extrudes the
// skinned vertex along its skinned normal by (shell index / shell count).
// Vertex layout / set 0 bindings are identical to model.vert.

#include "fur_shell_common.glsl"
#include "nawa_binding_common.glsl"

layout(location = 0) in vec3  inPosition;
layout(location = 1) in vec3  inNormal;
layout(location = 2) in vec2  inUV;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4  inWeights;

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

layout(std430, set = 0, binding = 2) readonly buffer BoneMatricesBuffer {
    mat4 bone_matrices[];
};

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragUV;
layout(location = 3) out flat uint fragInstanceID;
layout(location = 4) out float fragShellRatio;
layout(location = 5) out float fragBindGroove;

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

    vec3 skinnedPos    = (skinMat * vec4(inPosition, 1.0)).xyz;
    vec3 skinnedNormal = normalize(mat3(skinMat) * inNormal);

    float shellRatio = furShellRatio();
    vec3 normalWS = normalize(mat3(inst.model) * skinnedNormal);
    // Rope binding: sample at the undisplaced surface so every shell agrees,
    // crush the nap under the rope, then apply the body dent/bulge.
    vec3 surfaceWS = (inst.model * vec4(skinnedPos, 1.0)).xyz;
    NawaBindSample bind = nawaSampleField(surfaceWS);
    vec3 displaced = furShellDisplace(skinnedPos, skinnedNormal, shellRatio * nawaNapCrush(bind));

    vec4 worldPos = inst.model * vec4(displaced, 1.0);
    worldPos.xyz += nawaOffsetWS(normalWS, bind);
    fragWorldPos   = worldPos.xyz;
    fragNormal     = normalWS;
    fragBindGroove = bind.groove;
    fragUV         = inUV;
    fragInstanceID = uint(gl_InstanceIndex);
    fragShellRatio = shellRatio;
    gl_Position    = proj * view * worldPos;
}
