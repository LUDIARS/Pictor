#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in float v_coverage;  // 1 at the outline, 0 at the band's outer edge
layout(location = 2) in float v_gain;
layout(location = 0) out vec4 out_color;

void main() {
    // Emboldened, the band is feather + embolden wide: solid up to the feather, then fading.
    out_color = vec4(v_color.rgb, v_color.a * clamp(v_coverage * v_gain, 0.0, 1.0));
}
