// Pictor — Bloom Bright-Pass Extraction
// Extracts pixels above a brightness threshold with a soft knee, so only
// bright HDR areas contribute to bloom.

#version 450

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;

layout(push_constant) uniform Params {
    float threshold;       // brightness cutoff
    float soft_threshold;  // soft knee width (0..1 of threshold)
    float _pad0;
    float _pad1;
};

void main() {
    vec3 c = texture(sceneColor, inUV).rgb;
    float brightness = max(c.r, max(c.g, c.b));

    // Soft knee: smooth transition around the threshold (UE4-style curve).
    float knee  = threshold * soft_threshold + 1e-5;
    float soft  = clamp(brightness - threshold + knee, 0.0, 2.0 * knee);
    soft        = soft * soft / (4.0 * knee + 1e-5);
    float weight = max(soft, brightness - threshold) / max(brightness, 1e-5);

    outColor = vec4(c * weight, 1.0);
}
