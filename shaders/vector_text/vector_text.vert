#version 450
// Pictor::vector_text — glyph meshes in em units, placed per instance.
// The feather band is pushed out in screen pixels so the edge stays the same
// width at any size or rotation.

layout(location = 0) in vec2  in_position;   // em, y-up
layout(location = 1) in vec2  in_offset;     // unit, away from the fill (0 for fill vertices)
layout(location = 2) in float in_coverage;
layout(location = 3) in vec4  in_linear;     // column-major 2x2: em -> screen px
layout(location = 4) in vec4  in_place;      // tx, ty, feather_px, embolden_px
layout(location = 5) in vec4  in_color;

layout(push_constant) uniform Push {
    vec2 viewport;  // px
} pc;

layout(location = 0) out vec4 v_color;
layout(location = 1) out float v_coverage;
layout(location = 2) out float v_gain;  // band width / feather: the band's inner part stays solid

void main() {
    mat2 linear = mat2(in_linear.xy, in_linear.zw);
    vec2 screen = linear * in_position + in_place.xy;
    float scale = sqrt(abs(determinant(linear)));
    if (scale > 0.0) {
        screen += (linear * in_offset) / scale * (in_place.z + in_place.w);
    }
    gl_Position = vec4(screen / pc.viewport * 2.0 - 1.0, 0.0, 1.0);
    v_color = in_color;
    v_coverage = in_coverage;
    v_gain = (in_place.z + in_place.w) / max(in_place.z, 1e-3);
}
