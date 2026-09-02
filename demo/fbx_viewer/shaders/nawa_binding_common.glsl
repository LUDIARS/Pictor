// Rope-binding deformation shared by model.vert / fur_shell.vert.
// With meta.x == 0 every function is the identity, so untied rendering is
// unchanged.
#ifndef NAWA_BINDING_COMMON_GLSL
#define NAWA_BINDING_COMMON_GLSL

#define NAWA_MAX_SEGMENTS 96

layout(std140, set = 0, binding = 3) uniform NawaBindUBO {
    vec4 segStart[NAWA_MAX_SEGMENTS];   // xyz start (world), w rope radius
    vec4 segEnd[NAWA_MAX_SEGMENTS];     // xyz end
    vec4 deform;     // x sink depth, y bulge height, z groove width, w bulge width
    vec4 surface;    // x fur crush, y seam emphasis, z unused, w smooth-min blend
    vec4 meta;       // x active segment count
} nawa;

struct NawaBindSample {
    float groove;   // 0..1 directly under the rope
    float ring;     // 0..1 on the bulge beside the groove
};

// Signed distance to the rope chain (surface 0, inside negative), with a
// smooth-min so joints and crossings merge into one deeper groove.
float nawaChainDistance(vec3 positionWS) {
    float blend = max(nawa.surface.w, 1e-4);
    float dist = 1e5;
    int count = min(int(nawa.meta.x), NAWA_MAX_SEGMENTS);
    for (int i = 0; i < count; ++i) {
        vec3 a = nawa.segStart[i].xyz;
        float radius = nawa.segStart[i].w;
        vec3 ab = nawa.segEnd[i].xyz - a;
        vec3 ap = positionWS - a;
        float h = clamp(dot(ap, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
        float segment = length(ap - ab * h) - radius;
        float w = clamp(0.5 + 0.5 * (dist - segment) / blend, 0.0, 1.0);
        dist = mix(dist, segment, w) - blend * w * (1.0 - w);
    }
    return dist;
}

NawaBindSample nawaSampleField(vec3 positionWS) {
    NawaBindSample s;
    s.groove = 0.0;
    s.ring = 0.0;
    if (nawa.meta.x < 0.5) return s;

    float dist = nawaChainDistance(positionWS);
    float grooveWidth = max(nawa.deform.z, 1e-4);
    s.groove = 1.0 - smoothstep(0.0, grooveWidth, dist);

    // Smooth hill peaking at the groove edge: volume pushed sideways.
    float ringT = clamp(1.0 - abs(dist - grooveWidth) / max(nawa.deform.w, 1e-4), 0.0, 1.0);
    s.ring = ringT * ringT * (3.0 - 2.0 * ringT);
    return s;
}

// Groove sinks inward, ring bulges outward (amounts already tightness-scaled).
vec3 nawaOffsetWS(vec3 normalWS, NawaBindSample s) {
    return normalWS * (nawa.deform.y * s.ring - nawa.deform.x * s.groove);
}

// Fur nap crush under the rope: multiplier on the shell extrusion.
float nawaNapCrush(NawaBindSample s) {
    return 1.0 - clamp(nawa.surface.x * s.groove, 0.0, 1.0);
}

// Cheap contact shading: darken the groove so the dent reads even where
// the silhouette does not change (normals are not recomputed).
float nawaSeamShade(float groove) {
    return 1.0 - nawa.surface.y * 0.45 * groove;
}

#endif
