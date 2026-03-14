// Pictor — Shadow Depth Fragment Shader
// For opaque objects this shader is a no-op (depth-only write).
// For alpha-tested materials, samples the albedo alpha and discards
// fragments below the cutoff threshold.

#version 450

// ============================================================
// Fragment Input
// ============================================================

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec3 fragWorldNormal;

// ============================================================
// Material Texture (only bound for alpha-tested materials)
// ============================================================

layout(set = 1, binding = 0) uniform sampler2D albedoMap;

// ============================================================
// Push Constants (shared with vertex shader)
// ============================================================

layout(push_constant) uniform ShadowPushConstants {
    mat4  model;
    mat4  normalMatrix;
    uint  cascadeIndex;
    uint  materialFlags;         // bit 0 = ALPHA_TEST
    float alphaCutoff;
    uint  pad0;
};

void main() {
    // Alpha test: discard transparent fragments
    if ((materialFlags & 1u) != 0u) {
        float alpha = texture(albedoMap, fragTexCoord).a;
        if (alpha < alphaCutoff) {
            discard;
        }
    }

    // Depth is written automatically by the fixed-function pipeline.
    // No color output needed for shadow depth pass.
}
