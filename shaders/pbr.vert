// Pictor — PBR Vertex Shader
// Standard vertex transform with TBN output for normal mapping.

#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

// ============================================================
// Vertex Input (location matches Pictor vertex layout)
// ============================================================

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;    // xyz = tangent, w = handedness (+1/-1)
layout(location = 3) in vec2 inTexCoord0;

// ============================================================
// Vertex Output
// ============================================================

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out vec3 fragNormal;
layout(location = 3) out vec3 fragTangent;
layout(location = 4) out vec3 fragBitangent;

void main() {
    vec4 worldPos = model * vec4(inPosition, 1.0);
    fragWorldPos  = worldPos.xyz;
    fragTexCoord  = inTexCoord0;

    // Transform normal and tangent to world space
    mat3 nMat    = mat3(normalMatrix);
    fragNormal   = normalize(nMat * inNormal);
    fragTangent  = normalize(nMat * inTangent.xyz);
    fragBitangent = cross(fragNormal, fragTangent) * inTangent.w;

    gl_Position = viewProjection * worldPos;
}
