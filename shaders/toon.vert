// Pictor — Toon Vertex Shader
// Standard transform + outline pass support via vertex extrusion.

#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

// ============================================================
// Specialization Constants
// ============================================================

// Set to 1 to enable outline extrusion pass
layout(constant_id = 0) const uint OUTLINE_PASS = 0;

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

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out vec3 fragNormal;
layout(location = 3) out vec3 fragViewDir;

// ============================================================
// Toon Outline Parameters (set = 1, binding = 6)
// ============================================================

layout(set = 1, binding = 6) uniform ToonParams {
    vec4  outlineColor;        // RGB = color, A = unused
    float outlineWidth;        // world-space extrusion width
    float pad5, pad6, pad7;
};

void main() {
    mat3 nMat = mat3(normalMatrix);
    vec3 worldNormal = normalize(nMat * inNormal);

    vec3 pos = inPosition;

    // Outline pass: extrude vertices along normal direction
    if (OUTLINE_PASS == 1) {
        pos += inNormal * outlineWidth;
    }

    vec4 worldPos = model * vec4(pos, 1.0);

    fragWorldPos = worldPos.xyz;
    fragTexCoord = inTexCoord0;
    fragNormal   = worldNormal;
    fragViewDir  = normalize(cameraPosition.xyz - worldPos.xyz);

    gl_Position = viewProjection * worldPos;
}
