// Pictor — Post-Process Final Composite
// One fullscreen pass that performs, in order:
//   0. chromatic aberration (radial RGB shift on the scene fetch)
//   1. additive bloom composite (HDR)
//   2. exposure + tone mapping (HDR -> LDR)
//   3. saturation
//   4. LUT color grading (neutral LUT strip)
//   5. vignette
//   6. gamma correction
//   7. film grain (post-gamma, luminance-weighted)
//
// set 0 binding 0: scene HDR color
// set 0 binding 1: blurred bloom texture
// set 0 binding 2: LUT strip (RGBA8, lut_size tall x lut_size*lut_size wide)
//
// alpha には最終輝度を書く — 後段 FXAA (fxaa.frag) が luma 計算を
// alpha 読みで済ませるため (grain 適用前の値。 grain はノイズなので
// エッジ検出へ混ぜない)。

#version 450

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;
layout(set = 0, binding = 1) uniform sampler2D bloomTex;
layout(set = 0, binding = 2) uniform sampler2D lutTex;

layout(push_constant) uniform Params {
    float bloom_intensity;
    uint  tonemap_op;          // 0=ACES 1=Reinhard 2=ReinhardExt 3=Uncharted2 4=Linear
    float exposure;
    float gamma;
    float white_point;
    float saturation;
    float vignette_intensity;
    float vignette_radius;
    float vignette_softness;
    float lut_intensity;
    float lut_size;
    float vig_r;
    float vig_g;
    float vig_b;
    float ca_intensity;      // 色収差の最大ずらし量 (0 = 無効)
    float ca_start_radius;   // 効き始めの正規化半径
    float grain_intensity;   // フィルムグレイン強度 (0 = 無効)
    float grain_response;    // 輝度依存 (明部で弱める) 0..1
    float grain_seed;        // 毎フレーム進むシード (静止ノイズ防止)
};

// 画面座標 + シードから決定的な擬似乱数を得る (定番の sin-fract ハッシュ)。
float grainHash(vec2 uv, float seed) {
    return fract(sin(dot(uv + seed, vec2(12.9898, 78.233))) * 43758.5453);
}

// ---- Tone mapping operators ----
vec3 toneMapACES(vec3 c) {
    const float a = 2.51, b = 0.03, cc = 2.43, d = 0.59, e = 0.14;
    return clamp((c * (a * c + b)) / (c * (cc * c + d) + e), 0.0, 1.0);
}
vec3 toneMapReinhard(vec3 c) {
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    return c * ((lum / (1.0 + lum)) / max(lum, 1e-4));
}
vec3 toneMapReinhardExt(vec3 c, float wp) {
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float numer = lum * (1.0 + lum / (wp * wp));
    return c * ((numer / (1.0 + lum)) / max(lum, 1e-4));
}
vec3 uncharted2Partial(vec3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
vec3 toneMapUncharted2(vec3 c) {
    vec3 curr = uncharted2Partial(c * 2.0);
    vec3 whiteScale = vec3(1.0) / uncharted2Partial(vec3(11.2));
    return curr * whiteScale;
}

// ---- LUT strip lookup (neutral LUT, size N) ----
vec3 applyLUT(vec3 color, float N) {
    color = clamp(color, 0.0, 1.0);
    float blue = color.b * (N - 1.0);
    float b0 = floor(blue);
    float b1 = min(b0 + 1.0, N - 1.0);
    float f  = blue - b0;

    float u_in = (0.5 + color.r * (N - 1.0)) / (N * N);
    float v    = (0.5 + color.g * (N - 1.0)) / N;

    vec3 s0 = texture(lutTex, vec2(b0 / N + u_in, v)).rgb;
    vec3 s1 = texture(lutTex, vec2(b1 / N + u_in, v)).rgb;
    return mix(s0, s1, f);
}

void main() {
    vec3 scene = texture(sceneColor, inUV).rgb;
    vec3 bloom = texture(bloomTex, inUV).rgb;

    // 0. chromatic aberration — R/B を放射方向へ逆向きにずらして再サンプル。
    //    中心 (ca_start_radius 以内) は効かせない。 bloom はずらさない。
    if (ca_intensity > 0.0) {
        vec2 fromCenter = inUV - 0.5;
        float radius = length(fromCenter) * 1.41421356;
        float amount = smoothstep(ca_start_radius, 1.0, radius)
                       * ca_intensity * 0.01;   // UV 単位へスケール
        if (amount > 0.0) {
            vec2 dir = fromCenter * amount;
            scene.r = texture(sceneColor, inUV + dir).r;
            scene.b = texture(sceneColor, inUV - dir).b;
        }
    }

    // 1. additive bloom composite (HDR space)
    vec3 color = scene + bloom * bloom_intensity;

    // 2. exposure + tone mapping
    color *= exposure;
    switch (tonemap_op) {
        case 0u: color = toneMapACES(color);                       break;
        case 1u: color = toneMapReinhard(color);                   break;
        case 2u: color = toneMapReinhardExt(color, white_point);   break;
        case 3u: color = toneMapUncharted2(color);                 break;
        default: color = clamp(color, 0.0, 1.0);                   break;
    }

    // 3. saturation
    float gray = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(gray), color, saturation);
    color = clamp(color, 0.0, 1.0);

    // 4. LUT color grading
    if (lut_intensity > 0.0) {
        vec3 graded = applyLUT(color, lut_size);
        color = mix(color, graded, lut_intensity);
    }

    // 5. vignette
    if (vignette_intensity > 0.0) {
        vec2 d = inUV - 0.5;
        float dist = length(d) * 1.41421356;  // 0 center .. ~1 corner
        float edge = smoothstep(vignette_radius,
                                vignette_radius + vignette_softness, dist);
        color = mix(color, vec3(vig_r, vig_g, vig_b), edge * vignette_intensity);
    }

    // 6. gamma correction (linear -> display)
    color = pow(max(color, 0.0), vec3(1.0 / gamma));

    // FXAA 用の輝度 (grain 適用前 — ノイズをエッジ検出へ混ぜない)。
    float outLuma = dot(color, vec3(0.2126, 0.7152, 0.0722));

    // 7. film grain — 輝度依存の減衰付きで加算 (post-gamma)。
    if (grain_intensity > 0.0) {
        float noise = grainHash(inUV, grain_seed) - 0.5;
        float lumaWeight = 1.0 - outLuma * grain_response;
        color += noise * grain_intensity * 0.1 * lumaWeight;
        color = clamp(color, 0.0, 1.0);
    }

    outColor = vec4(color, outLuma);
}
