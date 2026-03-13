// Pictor — PBR Fragment Shader
// Cook-Torrance BRDF with metallic-roughness workflow.
// Supports: albedo, normal, metallic-roughness, AO, emissive maps.

#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

// ============================================================
// Fragment Input
// ============================================================

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec3 fragTangent;
layout(location = 4) in vec3 fragBitangent;

// ============================================================
// Material Textures (set = 1)
// ============================================================

layout(set = 1, binding = 0) uniform sampler2D albedoMap;
layout(set = 1, binding = 1) uniform sampler2D normalMap;
layout(set = 1, binding = 2) uniform sampler2D metallicRoughnessMap; // G = roughness, B = metallic
layout(set = 1, binding = 3) uniform sampler2D aoMap;
layout(set = 1, binding = 4) uniform sampler2D emissiveMap;

// ============================================================
// Material Parameters (set = 1, binding = 5)
// ============================================================

layout(set = 1, binding = 5) uniform MaterialParams {
    vec4  baseColorFactor;     // linear-space RGBA multiplier
    float metallicFactor;
    float roughnessFactor;
    float aoStrength;
    float emissiveStrength;
    vec4  emissiveFactor;      // RGB emissive multiplier
};

// ============================================================
// Output
// ============================================================

layout(location = 0) out vec4 outColor;

// ============================================================
// GGX / Cook-Torrance BRDF
// ============================================================

// GGX Normal Distribution Function
float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom + EPSILON);
}

// Smith's geometry function (Schlick-GGX)
float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k + EPSILON);
}

// Smith's method for combined geometry obstruction
float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return geometrySchlickGGX(NdotV, roughness) *
           geometrySchlickGGX(NdotL, roughness);
}

// ============================================================
// Lighting Calculation
// ============================================================

vec3 evaluateBRDF(vec3 N, vec3 V, vec3 L, vec3 radiance,
                  vec3 albedo, float metallic, float roughness) {
    vec3 H = normalize(V + L);

    // Dielectric F0 = 0.04, metallic F0 = albedo
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // Cook-Torrance specular
    float D = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3  F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);

    vec3 numerator  = D * G * F;
    float denominator = 4.0 * NdotV * NdotL + EPSILON;
    vec3 specular = numerator / denominator;

    // Energy conservation: diffuse is reduced by specular reflection
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

    // Lambertian diffuse
    vec3 diffuse = kD * albedo * INV_PI;

    return (diffuse + specular) * radiance * NdotL;
}

void main() {
    // Sample material textures
    vec4 albedoSample = texture(albedoMap, fragTexCoord);
    vec3 albedo       = srgbToLinear(albedoSample.rgb) * baseColorFactor.rgb;
    float alpha       = albedoSample.a * baseColorFactor.a;

    vec2 mr       = texture(metallicRoughnessMap, fragTexCoord).gb;
    float metallic  = mr.y * metallicFactor;
    float roughness = mr.x * roughnessFactor;
    roughness = clamp(roughness, 0.04, 1.0);

    float ao       = mix(1.0, texture(aoMap, fragTexCoord).r, aoStrength);
    vec3 emissive  = srgbToLinear(texture(emissiveMap, fragTexCoord).rgb)
                     * emissiveFactor.rgb * emissiveStrength;

    // Reconstruct TBN normal
    vec3 N = fragNormal;
    vec3 T = fragTangent;
    vec3 B = fragBitangent;
    vec3 mapN = texture(normalMap, fragTexCoord).rgb * 2.0 - 1.0;
    N = perturbNormal(normalize(N), normalize(T), normalize(B), mapN);

    vec3 V = normalize(cameraPosition.xyz - fragWorldPos);

    // ---- Accumulate lighting ----
    vec3 Lo = vec3(0.0);

    // Directional light (sun)
    {
        vec3 L = normalize(sun.direction.xyz);
        vec3 radiance = sun.color.rgb * sun.color.a;
        Lo += evaluateBRDF(N, V, L, radiance, albedo, metallic, roughness);
    }

    // Point lights
    for (uint i = 0; i < activePointLights; i++) {
        vec3 lightVec = pointLights[i].position.xyz - fragWorldPos;
        float dist    = length(lightVec);
        vec3 L        = lightVec / dist;

        float atten   = attenuatePoint(dist, pointLights[i].position.w);
        vec3 radiance = pointLights[i].color.rgb * pointLights[i].color.a * atten;

        Lo += evaluateBRDF(N, V, L, radiance, albedo, metallic, roughness);
    }

    // Ambient (simplified IBL substitute)
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 kS = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    vec3 kD = (1.0 - kS) * (1.0 - metallic);
    vec3 ambient = (kD * albedo + kS * 0.03) * ambientColor.rgb * ambientColor.a * ao;

    vec3 color = ambient + Lo + emissive;

    // Tone mapping and gamma correction
    color = toneMapACES(color);
    color = linearToSrgb(color);

    outColor = vec4(color, alpha);
}
