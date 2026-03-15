// Pictor — Tone Mapping Fragment Shader
// Maps HDR color to displayable LDR range with multiple operator choices.
// Also applies exposure, saturation adjustment, and gamma correction.

#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrInput;

layout(push_constant) uniform ToneMapParams {
    uint  tonemapOperator;   // 0=ACES, 1=Reinhard, 2=ReinhardExt, 3=Uncharted2, 4=Linear
    float exposure;
    float gamma;
    float whitePoint;
    float saturation;
};

// ---- Tone Mapping Operators ----

// ACES filmic (Narkowicz 2015 approximation)
vec3 toneMapACES(vec3 color) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

// Simple Reinhard (luminance-based)
vec3 toneMapReinhard(vec3 color) {
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float mapped = lum / (1.0 + lum);
    return color * (mapped / max(lum, 0.0001));
}

// Extended Reinhard with white point
vec3 toneMapReinhardExtended(vec3 color, float wp) {
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float wp2 = wp * wp;
    float numer = lum * (1.0 + lum / wp2);
    float mapped = numer / (1.0 + lum);
    return color * (mapped / max(lum, 0.0001));
}

// Uncharted 2 / Hable filmic helper
vec3 uncharted2Partial(vec3 x) {
    const float A = 0.15;  // Shoulder strength
    const float B = 0.50;  // Linear strength
    const float C = 0.10;  // Linear angle
    const float D = 0.20;  // Toe strength
    const float E = 0.02;  // Toe numerator
    const float F = 0.30;  // Toe denominator
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 toneMapUncharted2(vec3 color) {
    const float W = 11.2;  // Linear white point
    vec3 curr = uncharted2Partial(color * 2.0);
    vec3 whiteScale = vec3(1.0) / uncharted2Partial(vec3(W));
    return curr * whiteScale;
}

void main() {
    vec3 color = texture(hdrInput, inUV).rgb;

    // Apply exposure
    color *= exposure;

    // Apply selected tone mapping operator
    switch (tonemapOperator) {
        case 0u: color = toneMapACES(color);                          break;
        case 1u: color = toneMapReinhard(color);                      break;
        case 2u: color = toneMapReinhardExtended(color, whitePoint);  break;
        case 3u: color = toneMapUncharted2(color);                    break;
        case 4u: color = clamp(color, 0.0, 1.0);                     break;
    }

    // Saturation adjustment (post-tonemap)
    float gray = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(gray), color, saturation);

    // Gamma correction (linear → sRGB)
    color = pow(color, vec3(1.0 / gamma));

    outColor = vec4(color, 1.0);
}
