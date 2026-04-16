#version 450

// ============================================================
// Pictor General-Purpose Lit Fragment Shader
//
// Simple Lambert diffuse + ambient + optional half-Lambert wrap
// for pleasant appearance without HDR/tonemapping setup.
// Pairs with shaders/lit.vert.
// ============================================================

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in flat uint fragInstanceID;

layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;     // xyz = direction TOWARD light, w = intensity
    vec4 lightColor;   // rgb = light color, a = ambient intensity
    vec4 cameraPos;
};

struct InstanceData {
    mat4 model;
    vec4 baseColor;
    uvec4 skinInfo;
};
layout(std430, set = 0, binding = 1) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

layout(location = 0) out vec4 outColor;

void main() {
    InstanceData inst = instances[fragInstanceID];

    vec3 N = normalize(fragNormal);
    vec3 L = normalize(lightDir.xyz);

    // Half-Lambert: keeps shadowed side from going pitch black.
    float NdotL = dot(N, L);
    float halfLambert = NdotL * 0.5 + 0.5;
    float wrap = mix(max(NdotL, 0.0), halfLambert, 0.25);

    vec3 albedo = inst.baseColor.rgb;
    vec3 ambient = albedo * lightColor.a;
    vec3 diffuse = albedo * lightColor.rgb * wrap * lightDir.w;

    outColor = vec4(ambient + diffuse, inst.baseColor.a);
}
