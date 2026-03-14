// Pictor — Post-Process Demo Scene Fragment Shader
// Simple PBR lighting that outputs HDR values (>1.0) to demonstrate
// bloom, tone mapping, and other post-process effects.

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
    float pad0, pad1, pad2;
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

const float PI = 3.14159265358979;

// Fresnel-Schlick
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// GGX/Trowbridge-Reitz NDF
float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

// Smith's geometry function (Schlick-GGX)
float geometrySmith(float NdotV, float NdotL, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    float ggx1 = NdotV / (NdotV * (1.0 - k) + k);
    float ggx2 = NdotL / (NdotL * (1.0 - k) + k);
    return ggx1 * ggx2;
}

void main() {
    InstanceData inst = instances[fragInstanceID];

    vec3 baseColor  = inst.baseColor.rgb;
    float metallic  = inst.pbrParams.x;
    float roughness = inst.pbrParams.y;
    float ao        = inst.pbrParams.z;
    float emStr     = inst.pbrParams.w;
    vec3 emissive   = inst.emissiveColor.rgb;

    vec3 N = normalize(fragNormal);
    vec3 V = normalize(cameraPosition.xyz - fragWorldPos);
    vec3 L = normalize(sunDirection.xyz);
    vec3 H = normalize(V + L);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);

    // F0: dielectric = 0.04, metallic = baseColor
    vec3 F0 = mix(vec3(0.04), baseColor, metallic);

    // Cook-Torrance BRDF
    float NDF = distributionGGX(N, H, roughness);
    float G   = geometrySmith(NdotV, NdotL, roughness);
    vec3  F   = fresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 numerator = NDF * G * F;
    float denominator = 4.0 * NdotV * NdotL + 0.0001;
    vec3 specular = numerator / denominator;

    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

    // HDR lighting — sun intensity can produce values > 1.0
    float sunIntensity = sunColor.a;
    vec3 Lo = (kD * baseColor / PI + specular) * sunColor.rgb * sunIntensity * NdotL;

    // Ambient
    vec3 ambient = ambientColor.rgb * ambientColor.a * baseColor * ao;

    // Emissive (HDR: can be very bright for bloom to pick up)
    vec3 emission = emissive * emStr;

    vec3 color = ambient + Lo + emission;

    // Output HDR color (no tone mapping here — post-process handles it)
    outColor = vec4(color, 1.0);
}
