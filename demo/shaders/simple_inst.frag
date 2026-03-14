#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragWorldPos;
layout(location = 2) in flat uint fragInstanceID;

layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;
};

layout(location = 0) out vec4 outColor;

// Hash-based color per instance
vec3 instanceColor(uint id) {
    uint h = id * 2654435761u;
    float r = float((h >>  0) & 0xFFu) / 255.0;
    float g = float((h >>  8) & 0xFFu) / 255.0;
    float b = float((h >> 16) & 0xFFu) / 255.0;
    return mix(vec3(0.3, 0.4, 0.6), vec3(r, g, b), 0.5);
}

void main() {
    vec3 N = normalize(fragNormal);
    vec3 L = normalize(lightDir.xyz);
    float NdotL = max(dot(N, L), 0.0);

    vec3 baseColor = instanceColor(fragInstanceID);
    vec3 ambient = baseColor * 0.15;
    vec3 diffuse = baseColor * NdotL * 0.85;

    outColor = vec4(ambient + diffuse, 1.0);
}
