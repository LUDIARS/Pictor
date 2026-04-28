#version 450

// ============================================================
// Pictor Hologram Fragment Shader
//
// Stylised "holographic projection" look for SkillBox / Equipment
// cubes. Combines three effects:
//
//   1. Fresnel edge glow — view-angle dependent rim brightening,
//      `pow(1 - dot(N, V), kFresnelPower)`. Gives the silhouette a
//      bright outline.
//
//   2. Scan-line mask — sin() of world-space Y (frequency ~30 cycles
//      per unit), modulated over time so lines drift downward. Carves
//      thin dim bands across the surface.
//
//   3. Soft alpha — base alpha is multiplied by `(0.45 + 0.55*fresnel)`
//      so the interior is semi-transparent and the edge stays opaque,
//      reading correctly when blended on top of the scene.
//
// Pairs with shaders/hologram.vert (same SceneUBO / InstanceData
// layout as lit.{vert,frag}).
//
// Tuning constants are in the `// ── tunable ──` block below.
// ============================================================

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in flat uint fragInstanceID;

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

// Optional scan-line scrolling: tied to `lightDir.w` doubling as a
// time channel when no dedicated push constant is bound. When the
// caller doesn't update `lightDir.w` (pure static scene) the bands
// just sit still — the effect is still visible.
layout(push_constant) uniform Push {
    float u_time;
} push;

layout(location = 0) out vec4 outColor;

// ── tunable ──
const float kFresnelPower    = 2.5;   // higher = thinner rim
const float kScanFrequency   = 28.0;  // bands per world-space unit
const float kScanScrollSpeed = 0.6;   // bands per second
const float kScanDarken      = 0.40;  // 0 = no bands, 1 = full black bands
const vec3  kEdgeTint        = vec3(1.05, 1.20, 1.40);  // cool rim light

void main() {
    InstanceData inst = instances[fragInstanceID];

    vec3 N = normalize(fragNormal);
    vec3 V = normalize(cameraPos.xyz - fragWorldPos);

    // 1) Fresnel — bright at glancing angles.
    float fresnel = pow(1.0 - clamp(dot(N, V), 0.0, 1.0), kFresnelPower);

    // 2) Scan-line mask. Use world-space Y so the bands stay stable
    //    in 3D as the camera moves; scroll downward over time.
    float scan = sin((fragWorldPos.y - push.u_time * kScanScrollSpeed)
                     * kScanFrequency * 6.2831853);
    // map sin()∈[-1,1] → mask∈[1-kScanDarken, 1.0] with a soft falloff.
    float bandMask = 1.0 - kScanDarken * smoothstep(0.0, 1.0, max(0.0, -scan));

    // 3) Compose. Interior is the cube's base colour at reduced
    //    intensity; the rim is tinted bright with the edge colour.
    vec3 interior = inst.baseColor.rgb * 0.85;
    vec3 rim      = kEdgeTint * fresnel;
    vec3 rgb      = (interior + rim) * bandMask;

    // Soft alpha: edges read solidly, inside sees through.
    float alpha = inst.baseColor.a * (0.40 + 0.60 * fresnel);

    outColor = vec4(rgb, alpha);
}
