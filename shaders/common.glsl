// Pictor — Common Shader Utilities
// Shared constants and functions used by PBR and Toon shaders.
// Include via: #include "common.glsl" (requires GL_GOOGLE_include_directive)

#ifndef PICTOR_COMMON_GLSL
#define PICTOR_COMMON_GLSL

// ============================================================
// Constants
// ============================================================

const float PI      = 3.14159265358979323846;
const float INV_PI  = 0.31830988618379067154;
const float EPSILON = 1e-6;

// ============================================================
// Scene Uniforms (set = 0, binding = 0)
// Common to all material shaders.
// ============================================================

struct DirectionalLight {
    vec4 direction;   // xyz = direction (world space, toward light), w = unused
    vec4 color;       // rgb = color, a = intensity
};

struct PointLight {
    vec4 position;    // xyz = position, w = radius
    vec4 color;       // rgb = color, a = intensity
};

layout(set = 0, binding = 0) uniform SceneParams {
    mat4  view;
    mat4  projection;
    mat4  viewProjection;
    vec4  cameraPosition;       // xyz = world pos, w = unused
    vec4  ambientColor;         // rgb = ambient, a = intensity
    DirectionalLight sun;
    PointLight pointLights[4];
    uint  activePointLights;
    float time;
    float pad0, pad1;
};

// ============================================================
// Per-Object Push Constants
// ============================================================

layout(push_constant) uniform ObjectParams {
    mat4  model;
    mat4  normalMatrix;         // transpose(inverse(model)), upper 3x3
    uint  materialId;
    uint  pad2, pad3, pad4;
};

// ============================================================
// Utility Functions
// ============================================================

// Reconstruct world-space normal from tangent-space normal map sample
vec3 perturbNormal(vec3 N, vec3 T, vec3 B, vec3 mapNormal) {
    return normalize(T * mapNormal.x + B * mapNormal.y + N * mapNormal.z);
}

// Schlick approximation for Fresnel reflectance
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Schlick approximation with roughness for IBL
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0)
               * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// sRGB <-> Linear conversion
vec3 srgbToLinear(vec3 srgb) {
    return pow(srgb, vec3(2.2));
}

vec3 linearToSrgb(vec3 linear) {
    return pow(linear, vec3(1.0 / 2.2));
}

// ACES filmic tone mapping
vec3 toneMapACES(vec3 color) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e),
                 0.0, 1.0);
}

// Point light attenuation (inverse-square with radius falloff)
float attenuatePoint(float distance, float radius) {
    float d2 = distance * distance;
    float r2 = radius * radius;
    float attenuation = 1.0 / (d2 + 1.0);
    float falloff = clamp(1.0 - (d2 / r2), 0.0, 1.0);
    return attenuation * falloff * falloff;
}

#endif // PICTOR_COMMON_GLSL
