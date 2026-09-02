// Shared between fur_shell.vert / fur_shell.frag.
// Strand mask, shell ratio, and displacement for the FBX viewer's
// push-constant fur material.
#ifndef FUR_SHELL_COMMON_GLSL
#define FUR_SHELL_COMMON_GLSL

layout(push_constant) uniform FurPush {
    vec4 tipColor;   // rgb
    vec4 rimColor;   // rgb, w = rim power
    vec4 bend;       // xyz bend (model units), w = fur length
    vec4 shape;      // x density, y root thickness, z tip thickness, w root occlusion
    vec4 shell;      // x shell index, y shell count, z ground bounce
} fur;

// Shared by colour + (future) depth passes so the clipped silhouette matches.
const float kFurClipThreshold = 0.45;

float furShellRatio() {
    return clamp(fur.shell.x / max(fur.shell.y, 1.0), 0.0, 1.0);
}

vec2 hash22(vec2 cell) {
    vec3 v = fract(vec3(cell.xyx) * vec3(0.1031, 0.1030, 0.0973));
    v += dot(v, v.yzx + 33.33);
    return fract((v.xx + v.yz) * v.zy);
}

float strandMask(vec2 uv, float shellRatio) {
    vec2 strandUv = uv * fur.shape.x;
    vec2 cell  = floor(strandUv);
    vec2 local = fract(strandUv);
    vec2 strandCenter = mix(vec2(0.18), vec2(0.82), hash22(cell));
    float strandRadius = mix(fur.shape.y, fur.shape.z, shellRatio);
    float d = length(local - strandCenter);
    float primary = 1.0 - smoothstep(strandRadius * 0.72, strandRadius, d);

    vec2 secondaryCenter = 1.0 - strandCenter;
    float secondary = 1.0 - smoothstep(strandRadius * 0.45, strandRadius * 0.68,
                                       length(local - secondaryCenter));
    return max(primary, secondary * (1.0 - shellRatio * 0.75));
}

// Shell extrusion in the (skinned) model space: along the normal by
// length * ratio, plus a quadratic droop so tips sag under gravity.
vec3 furShellDisplace(vec3 position, vec3 normal, float shellRatio) {
    return position
         + normal * (fur.bend.w * shellRatio)
         + fur.bend.xyz * shellRatio * shellRatio;
}

#endif
