#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;   // xyz = direction toward light
};

// Instance transforms stored as position (xyz) + scale (w)
layout(std430, set = 0, binding = 1) readonly buffer InstanceBuffer {
    vec4 instances[];
};

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec3 fragWorldPos;
layout(location = 2) out flat uint fragInstanceID;

void main() {
    vec4 inst = instances[gl_InstanceIndex];
    vec3 worldPos = inPosition * inst.w + inst.xyz;
    fragWorldPos = worldPos;
    fragNormal = inNormal;
    fragInstanceID = gl_InstanceIndex;
    gl_Position = proj * view * vec4(worldPos, 1.0);
}
