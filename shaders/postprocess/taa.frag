// Pictor — Temporal Anti-Aliasing (TAA) fullscreen pass
// HDR 空間 (bloom / tonemap 前) で走る。 現フレームのシーンカラーと
// 前フレームの TAA 出力 (history) を、 カメラ再投影 + YCoCg AABB クランプで
// 混合する。 ジッタはホストが投影行列へ適用済み (taa_jitter() 契約)。
//
// binding 0 = 現フレームカラー (HDR)、 binding 1 = シーン深度、
// binding 2 = history (前フレームの pp_taa)。
// valid = 0 (初回 / カット / resize 後) のとき現フレームを素通しして
// history を再シードする (このフレームの出力がそのまま次の history になる)。
//
// per-object velocity は無い (phase 3) — カメラ再投影のみ。 動体の残像は
// クランプで抑制する。

#version 450

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D currentColor;
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;
layout(set = 0, binding = 2) uniform sampler2D historyColor;

layout(push_constant) uniform TaaPC {
    mat4  reproj;          // ジッタ無しの prevVP * inverse(currVP)
    float feedback_min;    // 動画素の history 混合率
    float feedback_max;    // 静止画素の history 混合率
    float jitter_x;        // 現フレームのジッタ (px)
    float jitter_y;
    float texel_x;
    float texel_y;
    uint  valid;
    uint  _pad0;
};

vec3 rgbToYCoCg(vec3 c) {
    return vec3( 0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                 0.5  * c.r             - 0.5  * c.b,
                -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
vec3 yCoCgToRgb(vec3 c) {
    return vec3(c.x + c.y - c.z,
                c.x       + c.z,
                c.x - c.y - c.z);
}

void main() {
    // ジッタを外した UV で現フレームをサンプル (エイリアスの位相を戻す)。
    vec2 unjitterUV = inUV - vec2(jitter_x * texel_x, jitter_y * texel_y);
    vec3 current = texture(currentColor, unjitterUV).rgb;

    if (valid == 0u) {
        outColor = vec4(current, 1.0);
        return;
    }

    // 深度から現フレーム NDC を再構築し、 前フレーム UV へ再投影。
    float depth = texture(sceneDepth, inUV).r;
    vec4 ndc  = vec4(inUV * 2.0 - 1.0, depth, 1.0);
    vec4 prev = ndc * reproj;                       // row-vector (v * M)
    prev.xyz /= max(abs(prev.w), 1e-6) * sign(prev.w);
    vec2 prevUV = prev.xy * 0.5 + 0.5;

    // 画面外へ出た history は使えない — 現フレームへ縮退。
    if (prevUV != clamp(prevUV, vec2(0.0), vec2(1.0))) {
        outColor = vec4(current, 1.0);
        return;
    }

    vec3 history = texture(historyColor, prevUV).rgb;

    // 3x3 近傍の YCoCg AABB で history をクランプ (ghosting 抑制)。
    vec3 mn = rgbToYCoCg(current);
    vec3 mx = mn;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            vec2 uv = unjitterUV + vec2(float(dx) * texel_x,
                                        float(dy) * texel_y);
            vec3 c = rgbToYCoCg(texture(currentColor, uv).rgb);
            mn = min(mn, c);
            mx = max(mx, c);
        }
    }
    vec3 histYcc = clamp(rgbToYCoCg(history), mn, mx);
    history = yCoCgToRgb(histYcc);

    // 再投影距離 (スクリーン速度) が大きいほど history を弱める。
    float velocity = length((inUV - prevUV) / vec2(texel_x, texel_y)); // px
    float feedback = mix(feedback_max, feedback_min,
                         clamp(velocity / 8.0, 0.0, 1.0));

    vec3 result = mix(current, history, feedback);
    outColor = vec4(result, 1.0);
}
