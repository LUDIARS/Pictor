// Pictor — Ocean Wave Fragment Shader
// Physically-based ocean surface shading with:
//   - Deep water color absorption (Beer-Lambert)
//   - Fresnel reflectance (Schlick)
//   - Sun specular highlights
//   - Subsurface scattering approximation
//   - Foam rendering at wave crests

#version 450

layout(set = 0, binding = 0) uniform SceneParams {
    mat4  view;
    mat4  projection;
    mat4  viewProjection;
    vec4  cameraPosition;
    float time;
    float maxTessLevel;
    float minTessLevel;
    float tessNearDist;
    float tessFarDist;
    float pad0, pad1, pad2;
};

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in float fragFoam;

layout(location = 0) out vec4 outColor;

// ============================================================
// Constants
// ============================================================

const vec3 SUN_DIRECTION = normalize(vec3(0.5, 0.6, 0.3));
const vec3 SUN_COLOR = vec3(1.0, 0.95, 0.85) * 3.0;

// Ocean colors (linear space)
const vec3 DEEP_WATER_COLOR = vec3(0.005, 0.02, 0.06);
const vec3 SHALLOW_WATER_COLOR = vec3(0.01, 0.08, 0.12);
const vec3 SCATTER_COLOR = vec3(0.02, 0.12, 0.08);
const vec3 FOAM_COLOR = vec3(0.85, 0.90, 0.92);

const vec3 SKY_ZENITH = vec3(0.15, 0.30, 0.60);
const vec3 SKY_HORIZON = vec3(0.55, 0.70, 0.85);

// ============================================================
// Helper Functions
// ============================================================

float fresnelSchlick(float cosTheta, float F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Simple sky color for reflection
vec3 skyColor(vec3 dir) {
    float t = max(dir.y, 0.0);
    return mix(SKY_HORIZON, SKY_ZENITH, t);
}

// ACES filmic tone mapping
vec3 toneMapACES(vec3 color) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

void main() {
    vec3 N = normalize(fragNormal);
    vec3 V = normalize(cameraPosition.xyz - fragWorldPos);
    vec3 L = SUN_DIRECTION;
    vec3 H = normalize(V + L);
    vec3 R = reflect(-V, N);

    float NdotV = max(dot(N, V), 0.001);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);

    // --- Fresnel (water IOR ~1.33, F0 ≈ 0.02) ---
    float fresnel = fresnelSchlick(NdotV, 0.02);

    // --- Reflection ---
    vec3 reflection = skyColor(R);

    // --- Water body color (depth-based tint) ---
    float depthFactor = clamp(fragWorldPos.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 waterColor = mix(DEEP_WATER_COLOR, SHALLOW_WATER_COLOR, depthFactor);

    // --- Subsurface scattering approximation ---
    // Light scatters through thin wave crests
    float sss = pow(max(dot(V, -L + N * 0.6), 0.0), 4.0) * 0.4;
    vec3 scatter = SCATTER_COLOR * sss * NdotL;

    // --- Sun specular (GGX-like highlight) ---
    float roughness = 0.05;
    float a2 = roughness * roughness;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159265 * denom * denom + 0.0001);
    vec3 specular = SUN_COLOR * D * fresnel * NdotL;

    // --- Combine ---
    vec3 color = mix(waterColor + scatter, reflection, fresnel) + specular;

    // --- Foam ---
    float foamMask = smoothstep(0.0, 0.5, fragFoam);
    color = mix(color, FOAM_COLOR, foamMask * 0.8);

    // --- Distance fog (blend to horizon) ---
    float dist = length(fragWorldPos - cameraPosition.xyz);
    float fogFactor = 1.0 - exp(-dist * 0.003);
    vec3 fogColor = SKY_HORIZON * 0.8;
    color = mix(color, fogColor, fogFactor);

    // --- Tone mapping & gamma ---
    color = toneMapACES(color);
    color = pow(color, vec3(1.0 / 2.2));

    outColor = vec4(color, 1.0);
}
