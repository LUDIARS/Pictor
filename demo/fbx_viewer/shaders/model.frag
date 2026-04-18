#version 450

// Lambert-diffuse fragment shader with a diffuse texture.
// Re-bound set 1 per submesh, while set 0 scene data is shared.

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragUV;
layout(location = 3) in flat uint fragInstanceID;

layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 cameraPos;
};

struct InstanceData {
    mat4  model;
    vec4  baseColor;
    uvec4 skinInfo;
};
layout(std430, set = 0, binding = 1) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

layout(set = 1, binding = 0) uniform sampler2D uDiffuse;

layout(location = 0) out vec4 outColor;

void main() {
    InstanceData inst = instances[fragInstanceID];

    vec3 N = normalize(fragNormal);
    vec3 L = normalize(lightDir.xyz);

    float NdotL = dot(N, L);
    float halfLambert = NdotL * 0.5 + 0.5;
    float wrap = mix(max(NdotL, 0.0), halfLambert, 0.25);

    vec4 tex = texture(uDiffuse, fragUV);
    vec3 albedo = tex.rgb * inst.baseColor.rgb;
    vec3 ambient = albedo * lightColor.a;
    vec3 diffuse = albedo * lightColor.rgb * wrap * lightDir.w;

    outColor = vec4(ambient + diffuse, tex.a * inst.baseColor.a);
}
