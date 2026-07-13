// Pictor — 疑似レンズフレア (ghost + halo) fullscreen pass
// bloom 抽出結果 (明部) を中心対称に反転サンプルして ghost 列を作り、
// 固定半径の halo を足す。 出力 = 入力 bloom + フレア (加算) — grade の
// bloom 入力を本 pass の出力へ差し替える形で割り込む (grade 無変更)。
// 光源データを要求しないスクリーンスペース近似 (phase 3)。
//
// binding 0 = bloom 抽出/チェーン結果。 intensity 0 で素通し (恒等縮退)。

#version 450

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D bloomTex;

layout(push_constant) uniform LensFlarePC {
    float intensity;
    float ghost_spacing;
    float halo_radius;
    float halo_intensity;
    uint  ghost_count;      // 上限 8
    float texel_x;
    float texel_y;
    float _pad0;
};

void main() {
    vec3 bloom = texture(bloomTex, inUV).rgb;

    if (intensity <= 0.0) {
        outColor = vec4(bloom, 1.0);
        return;
    }

    // 中心対称の反転 UV (画面中心 0.5 まわり)。
    vec2 flipUV = vec2(1.0) - inUV;
    vec2 toCenter = (vec2(0.5) - flipUV) * ghost_spacing;

    vec3 flare = vec3(0.0);

    // ghosts — 反転 UV から中心へ向かって等間隔にサンプル。
    uint ghosts = min(ghost_count, 8u);
    for (uint i = 0u; i < ghosts; ++i) {
        vec2 uv = flipUV + toCenter * float(i);
        // 画面端の ghost をフェード (中心に近いほど強い)。
        float w = 1.0 - length(vec2(0.5) - uv) * 1.41421356;
        w = max(w, 0.0);
        w = w * w;
        flare += texture(bloomTex, clamp(uv, vec2(0.0), vec2(1.0))).rgb * w;
    }
    flare /= float(max(ghosts, 1u));

    // halo — 中心から固定半径のリング方向にサンプル。
    if (halo_intensity > 0.0) {
        vec2 dir = normalize(vec2(0.5) - inUV + vec2(1e-5));
        vec2 haloUV = inUV + dir * halo_radius;
        float haloW = 1.0 - abs(length(vec2(0.5) - inUV) - halo_radius) * 8.0;
        haloW = max(haloW, 0.0);
        flare += texture(bloomTex, clamp(haloUV, vec2(0.0), vec2(1.0))).rgb
               * haloW * halo_intensity;
    }

    outColor = vec4(bloom + flare * intensity, 1.0);
}
