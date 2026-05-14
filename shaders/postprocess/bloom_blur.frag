// Pictor — Bloom Separable Gaussian Blur
// One axis per invocation (horizontal then vertical). 9-tap Gaussian.

#version 450

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcTex;

layout(push_constant) uniform Params {
    vec2  direction;  // texel step on the blur axis (1/w,0) or (0,1/h)
    float radius;     // blur radius multiplier
    float _pad0;
};

void main() {
    // Normalized 9-tap Gaussian weights (sigma ~ 2).
    const float w0 = 0.227027;
    const float w1 = 0.194594;
    const float w2 = 0.121622;
    const float w3 = 0.054054;
    const float w4 = 0.016216;

    vec2 step = direction * radius;
    vec3 acc = texture(srcTex, inUV).rgb * w0;
    acc += texture(srcTex, inUV + step * 1.0).rgb * w1;
    acc += texture(srcTex, inUV - step * 1.0).rgb * w1;
    acc += texture(srcTex, inUV + step * 2.0).rgb * w2;
    acc += texture(srcTex, inUV - step * 2.0).rgb * w2;
    acc += texture(srcTex, inUV + step * 3.0).rgb * w3;
    acc += texture(srcTex, inUV - step * 3.0).rgb * w3;
    acc += texture(srcTex, inUV + step * 4.0).rgb * w4;
    acc += texture(srcTex, inUV - step * 4.0).rgb * w4;

    outColor = vec4(acc, 1.0);
}
