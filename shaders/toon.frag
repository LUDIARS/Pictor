// Pictor — Toon Fragment Shader
// Cel-shading with configurable band count, rim lighting, and specular highlight.
// Outline pass outputs a solid color (combined with front-face culling).

#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

// ============================================================
// Specialization Constants
// ============================================================

layout(constant_id = 0) const uint OUTLINE_PASS = 0;

// ============================================================
// Fragment Input
// ============================================================

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec3 fragViewDir;

// ============================================================
// Material Textures (set = 1)
// ============================================================

layout(set = 1, binding = 0) uniform sampler2D albedoMap;
layout(set = 1, binding = 1) uniform sampler2D rampMap;         // 1D shading ramp (optional)

// ============================================================
// Material Parameters (set = 1, binding = 5)
// ============================================================

layout(set = 1, binding = 5) uniform MaterialParams {
    vec4  baseColorFactor;
    float shadowThreshold;     // NdotL threshold for shadow band (default 0.0)
    float shadowSmoothness;    // anti-alias width for shadow edge
    uint  bandCount;           // number of discrete shading bands (2-8)
    float specularSize;        // specular highlight size (0.0-1.0)
    float specularSmoothness;  // specular edge smoothness
    float rimPower;            // rim light exponent
    float rimStrength;         // rim light intensity
    float pad8;
};

// ============================================================
// Toon Outline Parameters (set = 1, binding = 6)
// ============================================================

layout(set = 1, binding = 6) uniform ToonParams {
    vec4  outlineColor;
    float outlineWidth;
    float pad5, pad6, pad7;
};

// ============================================================
// Output
// ============================================================

layout(location = 0) out vec4 outColor;

// ============================================================
// Toon Shading Functions
// ============================================================

// Quantize a value into discrete bands
float quantize(float value, uint bands) {
    float stepped = floor(value * float(bands)) / float(bands);
    return stepped;
}

// Smooth step between shadow and lit regions
float toonDiffuse(float NdotL) {
    // Method 1: Band-based quantization
    float lit = clamp((NdotL - shadowThreshold) / (shadowSmoothness + EPSILON)
                      * 0.5 + 0.5, 0.0, 1.0);
    return quantize(lit, bandCount);
}

// Ramp-based shading (sample from 1D ramp texture)
float toonDiffuseRamp(float NdotL) {
    float u = NdotL * 0.5 + 0.5; // remap [-1,1] -> [0,1]
    return texture(rampMap, vec2(u, 0.5)).r;
}

// Toon specular highlight (hard-edged)
float toonSpecular(vec3 N, vec3 H) {
    float NdotH = max(dot(N, H), 0.0);
    float spec  = pow(NdotH, 1.0 / (specularSize + EPSILON));
    return smoothstep(0.5 - specularSmoothness, 0.5 + specularSmoothness, spec);
}

// Rim lighting (Fresnel-based edge glow)
float rimLight(vec3 N, vec3 V) {
    float NdotV = max(dot(N, V), 0.0);
    return pow(1.0 - NdotV, rimPower) * rimStrength;
}

void main() {
    // ---- Outline pass: solid color output ----
    if (OUTLINE_PASS == 1) {
        outColor = outlineColor;
        return;
    }

    // ---- Main toon shading ----
    vec3 N = normalize(fragNormal);
    vec3 V = normalize(fragViewDir);

    // Sample albedo
    vec4 albedoSample = texture(albedoMap, fragTexCoord);
    vec3 albedo = albedoSample.rgb * baseColorFactor.rgb;
    float alpha = albedoSample.a * baseColorFactor.a;

    vec3 Lo = vec3(0.0);

    // Directional light (sun)
    {
        vec3 L = normalize(sun.direction.xyz);
        vec3 H = normalize(V + L);
        float NdotL = dot(N, L);

        float diffuse = toonDiffuse(NdotL);
        float spec    = toonSpecular(N, H);

        vec3 lightColor = sun.color.rgb * sun.color.a;
        Lo += albedo * lightColor * diffuse;
        Lo += lightColor * spec;
    }

    // Point lights
    for (uint i = 0; i < activePointLights; i++) {
        vec3 lightVec = pointLights[i].position.xyz - fragWorldPos;
        float dist    = length(lightVec);
        vec3 L        = lightVec / dist;
        vec3 H        = normalize(V + L);
        float NdotL   = dot(N, L);

        float atten   = attenuatePoint(dist, pointLights[i].position.w);
        float diffuse = toonDiffuse(NdotL);
        float spec    = toonSpecular(N, H);

        vec3 lightColor = pointLights[i].color.rgb * pointLights[i].color.a * atten;
        Lo += albedo * lightColor * diffuse;
        Lo += lightColor * spec;
    }

    // Rim lighting
    float rim = rimLight(N, V);
    Lo += albedo * rim;

    // Ambient
    vec3 ambient = albedo * ambientColor.rgb * ambientColor.a;

    vec3 color = ambient + Lo;

    // Gamma correction (toon shading typically works in sRGB space directly)
    color = linearToSrgb(color);

    outColor = vec4(color, alpha);
}
