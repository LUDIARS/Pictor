// Pictor — Shadow Depth Vertex Shader
// Renders geometry from the light's perspective into the shadow atlas.
// Supports cascaded shadow maps (CSM) via push constant cascade index.
// Alpha-tested materials pass through UV for fragment discard.

#version 450
#extension GL_GOOGLE_include_directive : require

// ============================================================
// Cascade Uniforms (set = 0, binding = 0)
// ============================================================

layout(set = 0, binding = 0) uniform ShadowCascadeParams {
    mat4  lightViewProj[4];      // per-cascade light VP matrices
    vec4  cascadeSplits;         // view-space split distances
    uint  cascadeCount;
    float depthBias;
    float normalBias;
    float slopeScaleBias;
};

// ============================================================
// Per-Object Push Constants
// ============================================================

layout(push_constant) uniform ShadowPushConstants {
    mat4  model;
    mat4  normalMatrix;          // transpose(inverse(model)), upper 3x3
    uint  cascadeIndex;          // which cascade this draw targets
    uint  materialFlags;         // bit 0 = ALPHA_TEST, bit 1 = TWO_SIDED
    float alphaCutoff;
    uint  pad0;
};

// ============================================================
// Vertex Input
// ============================================================

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inTexCoord0;

// ============================================================
// Vertex Output
// ============================================================

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec3 fragWorldNormal;

void main() {
    vec4 worldPos = model * vec4(inPosition, 1.0);

    // Apply normal-based bias to reduce self-shadowing (peter-panning tradeoff)
    mat3 nMat = mat3(normalMatrix);
    vec3 worldNormal = normalize(nMat * inNormal);

    // Offset position along normal to reduce shadow acne
    worldPos.xyz += worldNormal * normalBias;

    fragTexCoord = inTexCoord0;
    fragWorldNormal = worldNormal;

    gl_Position = lightViewProj[cascadeIndex] * worldPos;

    // Apply slope-scale depth bias
    // Vulkan depth range [0, 1], apply constant bias in clip space
    gl_Position.z += depthBias + slopeScaleBias * abs(gl_Position.z);
    gl_Position.z = clamp(gl_Position.z, 0.0, gl_Position.w);
}
