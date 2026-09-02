#version 450
#extension GL_GOOGLE_include_directive : require

// Shell-fur fragment shader:
//   strand cut-out  → root→tip tint → wrapped diffuse + root occlusion
//   → pseudo-GI ground bounce → soft rim + back-scatter.
// Shadows / SH are replaced by the viewer's single directional light +
// constant ambient (lightColor.a).

#include "fur_shell_common.glsl"
#include "nawa_binding_common.glsl"

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragUV;
layout(location = 3) in flat uint fragInstanceID;
layout(location = 4) in float fragShellRatio;
layout(location = 5) in float fragBindGroove;

layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;    // xyz toward light, w intensity
    vec4 lightColor;  // rgb, a = ambient
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
    float shellRatio = fragShellRatio;
    float mask = strandMask(fragUV, shellRatio);
    if (mask < kFurClipThreshold) discard;

    InstanceData inst = instances[fragInstanceID];
    vec3 N = normalize(fragNormal);
    vec3 L = normalize(lightDir.xyz);
    vec3 V = normalize(cameraPos.xyz - fragWorldPos);

    vec4 tex = texture(uDiffuse, fragUV);
    vec3 furTint = mix(inst.baseColor.rgb, fur.tipColor.rgb, shellRatio * shellRatio);
    vec3 albedo = tex.rgb * furTint;

    float diffuse = clamp(dot(N, L), 0.0, 1.0) * 0.72 + 0.28;
    float rootOcclusion = mix(1.0 - fur.shape.w, 1.0, shellRatio);

    // Pseudo-GI: floor bounce into the downward hemisphere, sharing the root AO.
    vec3 groundBounce = lightColor.rgb * fur.shell.z * clamp(dot(N, vec3(0.0, -1.0, 0.0)), 0.0, 1.0);
    vec3 indirect = (lightColor.rgb * lightColor.a + groundBounce) * rootOcclusion;
    vec3 direct   = lightColor.rgb * diffuse * lightDir.w;

    // Velvet sheen: short pile scatters light back toward grazing angles,
    // brightest on the outer shells; plus a little back-scatter at the tips.
    float rim = pow(clamp(1.0 - dot(N, V), 0.0, 1.0), fur.rimColor.w);
    float backScatter = pow(clamp(dot(-N, L), 0.0, 1.0), 2.0) * shellRatio;
    vec3 softLight = fur.rimColor.rgb * (rim * (0.18 + 0.22 * shellRatio) + backScatter * 0.15);

    vec3 lit = (albedo * (indirect + direct) + softLight) * nawaSeamShade(fragBindGroove);
    outColor = vec4(lit, 1.0);
}
