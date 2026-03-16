// Pictor — Post-Process Demo Scene Fragment Shader
// PBR Cook-Torrance with HDR lighting, SSAO approximation,
// depth-based DoF simulation, and ACES tone mapping.

#version 450

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) flat in uint fragInstanceID;

layout(location = 0) out vec4 outColor;

// Scene uniforms
layout(set = 0, binding = 0) uniform SceneParams {
    mat4  view;
    mat4  projection;
    mat4  viewProjection;
    vec4  cameraPosition;
    vec4  ambientColor;

    vec4  sunDirection;
    vec4  sunColor;

    float time;
    float dofFocusDistance;
    float dofFocusRange;
    float exposure;
};

// Instance data
struct InstanceData {
    mat4 model;
    vec4 baseColor;
    vec4 pbrParams;       // metallic, roughness, ao, emissiveStrength
    vec4 emissiveColor;
};

layout(std430, set = 0, binding = 1) readonly buffer Instances {
    InstanceData instances[];
};

const float PI      = 3.14159265358979;
const float INV_PI  = 0.31830988618379;
const float EPSILON = 1e-6;

// ---- Fresnel ----
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0)
               * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ---- GGX/Trowbridge-Reitz NDF ----
float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom + EPSILON);
}

// ---- Smith's geometry function (Schlick-GGX) ----
float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k + EPSILON);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return geometrySchlickGGX(NdotV, roughness) *
           geometrySchlickGGX(NdotL, roughness);
}

// ---- BRDF evaluation ----
vec3 evaluateBRDF(vec3 N, vec3 V, vec3 L, vec3 radiance,
                  vec3 albedo, float metallic, float roughness) {
    vec3 H = normalize(V + L);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    float D = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3  F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);

    vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + EPSILON);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diffuse = kD * albedo * INV_PI;

    return (diffuse + specular) * radiance * NdotL;
}

// ---- SSAO approximation ----
float approximateAO(vec3 worldPos, vec3 N) {
    float groundDist = worldPos.y;
    float groundAO = clamp(groundDist * 0.5, 0.0, 1.0);
    float edgeAO = clamp(dot(N, vec3(0.0, 1.0, 0.0)) * 0.5 + 0.5, 0.3, 1.0);
    return groundAO * edgeAO;
}

// ---- Tone mapping ----
vec3 toneMapACES(vec3 color) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

vec3 linearToSrgb(vec3 linear) {
    return pow(linear, vec3(1.0 / 2.2));
}

void main() {
    InstanceData inst = instances[fragInstanceID];

    vec3  baseColor  = inst.baseColor.rgb;
    float metallic   = inst.pbrParams.x;
    float roughness  = clamp(inst.pbrParams.y, 0.04, 1.0);
    float ao         = inst.pbrParams.z;
    float emStr      = inst.pbrParams.w;
    vec3  emissive   = inst.emissiveColor.rgb;

    vec3 N = normalize(fragNormal);
    vec3 V = normalize(cameraPosition.xyz - fragWorldPos);

    // SSAO approximation
    float ssao = approximateAO(fragWorldPos, N);
    ao *= ssao;

    // ---- Accumulate lighting ----
    vec3 Lo = vec3(0.0);

    // Directional light (sun)
    {
        vec3 L = normalize(sunDirection.xyz);
        vec3 radiance = sunColor.rgb * sunColor.a;
        Lo += evaluateBRDF(N, V, L, radiance, baseColor, metallic, roughness);
    }

    // ---- Ambient / GI approximation ----
    vec3 F0 = mix(vec3(0.04), baseColor, metallic);
    vec3 kS = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    vec3 kD = (1.0 - kS) * (1.0 - metallic);

    // Hemisphere ambient: sky from above, ground bounce from below
    vec3 skyColor    = ambientColor.rgb * ambientColor.a;
    vec3 groundColor = skyColor * vec3(0.4, 0.35, 0.3);
    float skyFactor  = N.y * 0.5 + 0.5;
    vec3 hemisphereAmbient = mix(groundColor, skyColor, skyFactor);

    vec3 ambient = (kD * baseColor + kS * 0.03) * hemisphereAmbient * ao;

    // Emissive (HDR: bright values for bloom appearance)
    vec3 emission = emissive * emStr;

    vec3 color = ambient + Lo + emission;

    // ---- Exposure ----
    float exp_val = max(exposure, 0.1);
    color *= exp_val;

    // ---- DoF simulation (depth-based desaturation + blur fade) ----
    if (dofFocusDistance > 0.0) {
        float fragDist = length(cameraPosition.xyz - fragWorldPos);
        float focusRange = max(dofFocusRange, 0.5);
        float coc = clamp(abs(fragDist - dofFocusDistance) / focusRange, 0.0, 1.0);
        coc = smoothstep(0.0, 1.0, coc);

        // Out-of-focus: desaturate + slight fog
        float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
        vec3 desaturated = mix(color, vec3(luma), 0.4 * coc);
        vec3 fogColor = hemisphereAmbient * 0.3;
        color = mix(desaturated, mix(desaturated, fogColor, 0.15), coc);
    }

    // ---- Tone mapping + gamma ----
    color = toneMapACES(color);
    color = linearToSrgb(color);

    outColor = vec4(color, 1.0);
}
