#version 450

// Hemp rope: diagonal twist stripes over a warm base, wrapped Lambert.

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragUV;

layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 cameraPos;
};

layout(location = 0) out vec4 outColor;

void main() {
    vec3 N = normalize(fragNormal);
    vec3 L = normalize(lightDir.xyz);

    // Twist: stripes advance around the rope as they travel along it.
    float twist = fract(fragUV.x * 0.25 + fragUV.y * 2.0);
    float strand = smoothstep(0.0, 0.25, twist) * (1.0 - smoothstep(0.75, 1.0, twist));
    vec3 base = mix(vec3(0.55, 0.42, 0.26), vec3(0.80, 0.66, 0.44), strand);

    float ndl = dot(N, L);
    float wrap = mix(max(ndl, 0.0), ndl * 0.5 + 0.5, 0.3);
    vec3 ambient = base * lightColor.a;
    vec3 diffuse = base * lightColor.rgb * wrap * lightDir.w;
    outColor = vec4(ambient + diffuse, 1.0);
}
