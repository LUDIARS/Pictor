// Pictor — PBR Demo Vertex Shader
// Self-contained vertex shader for the graphics demo.
// Supports per-instance transforms via storage buffer for cube + floor geometry.

#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

// Scene uniforms
layout(set = 0, binding = 0) uniform SceneUBO {
    mat4  view;
    mat4  proj;
    mat4  viewProj;
    vec4  cameraPos;          // xyz = world pos
    vec4  ambientColor;       // rgb = ambient, a = intensity

    // Directional light (sun)
    vec4  sunDirection;       // xyz = direction toward light
    vec4  sunColor;           // rgb = color, a = intensity

    // Spotlight
    vec4  spotPosition;       // xyz = world position, w = unused
    vec4  spotDirection;      // xyz = direction, w = unused
    vec4  spotColor;          // rgb = color, a = intensity
    vec4  spotParams;         // x = innerCone, y = outerCone, z = range, w = unused

    // Shadow (CSM cascade 0 only for demo)
    mat4  shadowViewProj;
    vec4  shadowParams;       // x = bias, y = normalBias, z = strength, w = enabled

    float time;
    float pad0, pad1, pad2;
};

// Per-instance data: model matrix (4x vec4) + material params
struct InstanceData {
    mat4  model;
    vec4  baseColor;          // rgb = albedo, a = alpha
    vec4  pbrParams;          // x = metallic, y = roughness, z = ao, w = emissiveStrength
    vec4  emissiveColor;      // rgb = emissive
};

layout(std430, set = 0, binding = 1) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec4 fragBaseColor;
layout(location = 3) out vec4 fragPBRParams;
layout(location = 4) out vec4 fragEmissive;
layout(location = 5) out vec4 fragShadowCoord;

void main() {
    InstanceData inst = instances[gl_InstanceIndex];

    vec4 worldPos = inst.model * vec4(inPosition, 1.0);
    fragWorldPos  = worldPos.xyz;

    // Transform normal to world space (assuming uniform scale, use transpose-inverse for non-uniform)
    mat3 normalMat = mat3(inst.model);
    fragNormal     = normalize(normalMat * inNormal);

    // Pass material params to fragment shader
    fragBaseColor  = inst.baseColor;
    fragPBRParams  = inst.pbrParams;
    fragEmissive   = inst.emissiveColor;

    // Shadow coord (cascade 0)
    if (shadowParams.w > 0.5) {
        vec3 biasedPos = worldPos.xyz + fragNormal * shadowParams.y;
        vec4 lightClip = shadowViewProj * vec4(biasedPos, 1.0);
        fragShadowCoord = vec4(lightClip.xyz / lightClip.w, 1.0);
        fragShadowCoord.xy = fragShadowCoord.xy * 0.5 + 0.5;
    } else {
        fragShadowCoord = vec4(0.0, 0.0, 1.0, 0.0);
    }

    gl_Position = viewProj * worldPos;
}
