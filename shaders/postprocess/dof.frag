// Pictor — Depth of Field (fullscreen fragment pass)
// PostProcessChain 用の dof.comp 移植。 CoC 計算 + Poisson disc ブラー +
// 合成を 1 pass で行う。 binding 0 = シーンカラー (HDR)、
// binding 1 = シーン深度 (__depth__、 NEAREST サンプラ)。
// bokeh_radius = 0 (DoF 無効) のとき CoC が常に 0.5 未満になり素通しする。

#version 450

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;

layout(push_constant) uniform DofPC {
    float focus_distance;
    float focus_range;
    float bokeh_radius;
    float near_start;
    float near_end;
    float far_start;
    float far_end;
    uint  sample_count;
    float texel_x;
    float texel_y;
    float near_plane;   // > 0 で深度バッファ値を view 距離へ線形化
    float far_plane;    // 0 = 線形化なし (生の深度を距離扱い — 旧挙動)
};

// 深度バッファ値 → view 距離。 near/far 未設定 (0) は素通し (レガシー互換)。
float toViewDistance(float d) {
    if (far_plane <= near_plane || near_plane <= 0.0) return d;
    return near_plane * far_plane
         / max(far_plane - d * (far_plane - near_plane), 1e-6);
}

// Poisson disc samples (16 points, normalized to unit disc) — dof.comp と同一。
const vec2 poissonDisc[16] = vec2[](
    vec2(-0.94201,  -0.39906),
    vec2( 0.94558,  -0.76890),
    vec2(-0.09418,  -0.92938),
    vec2( 0.34495,   0.29387),
    vec2(-0.91588,   0.45771),
    vec2(-0.81544,  -0.87912),
    vec2( 0.19984,  -0.29614),
    vec2(-0.24188,   0.99706),
    vec2( 0.44323,   0.93427),
    vec2( 0.78583,   0.00399),
    vec2(-0.52389,  -0.22294),
    vec2( 0.36291,  -0.67380),
    vec2(-0.54532,   0.63536),
    vec2( 0.13980,   0.56616),
    vec2(-0.30654,  -0.60812),
    vec2( 0.74882,   0.56394)
);

float computeCoC(float depth) {
    // Near field (negative CoC)
    float nearCoC = smoothstep(near_end, near_start, depth);
    // Far field (positive CoC)
    float farCoC = smoothstep(far_start, far_end, depth);

    // Focus region
    float halfRange = focus_range * 0.5;
    if (depth >= focus_distance - halfRange && depth <= focus_distance + halfRange) {
        return 0.0;
    }

    return depth < focus_distance ? -nearCoC * bokeh_radius : farCoC * bokeh_radius;
}

void main() {
    vec2 texelSize = vec2(texel_x, texel_y);

    float centerDepth = toViewDistance(texture(sceneDepth, inUV).r);
    float coc = computeCoC(centerDepth);
    float absCoC = abs(coc);

    // If CoC is negligible, output sharp color
    vec3 sharp = texture(sceneColor, inUV).rgb;
    if (absCoC < 0.5) {
        outColor = vec4(sharp, 1.0);
        return;
    }

    // Variable-radius disc blur
    vec3 colorSum = vec3(0.0);
    float weightSum = 0.0;
    uint samples = min(sample_count, 16u);

    for (uint i = 0u; i < samples; ++i) {
        vec2 offset = poissonDisc[i] * absCoC * texelSize;
        vec2 sampleUV = inUV + offset;

        vec3 sampleColor = texture(sceneColor, sampleUV).rgb;
        float sampleDepth = toViewDistance(texture(sceneDepth, sampleUV).r);
        float sampleCoC = abs(computeCoC(sampleDepth));

        // Weight: accept sample if its CoC covers the center pixel
        float weight = smoothstep(0.0, 2.0, sampleCoC);
        colorSum += sampleColor * weight;
        weightSum += weight;
    }

    vec3 blurred = colorSum / max(weightSum, 0.001);

    // Blend between sharp and blurred based on CoC magnitude
    float blendFactor = smoothstep(0.5, 2.0, absCoC);
    outColor = vec4(mix(sharp, blurred, blendFactor), 1.0);
}
