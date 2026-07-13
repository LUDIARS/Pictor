// Pictor — Auto Exposure: 輝度計測 pass (1x1 viewport で 1 fragment のみ実行)
// シーンカラーを 8x8 の疎グリッドでサンプルし、 対数平均輝度を求めて
// 前フレームの適応値 (history) と時間混合する。 出力ターゲットは
// フル解像度確保だが viewport 1x1 — texel (0,0) だけが有効
// (exposure_apply / 次フレームの history がそこだけ読む)。
//
// binding 0 = シーンカラー (HDR)、 binding 1 = history (前フレームの適応輝度)。
// history が 0 (初期化直後の黒クリア) のときは即時追従する。

#version 450

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;
layout(set = 0, binding = 1) uniform sampler2D historyLum;

layout(push_constant) uniform ExposureMeasurePC {
    float min_lum;
    float max_lum;
    float blend;      // 1 - exp(-dt * adaptation_rate)
    float _pad0;
};

void main() {
    // 8x8 疎グリッドの対数平均輝度。 周辺より中央を重視する重み付き。
    float logSum = 0.0;
    float weightSum = 0.0;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            vec2 uv = (vec2(float(x), float(y)) + 0.5) / 8.0;
            vec3 c = texture(sceneColor, uv).rgb;
            float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
            // 中央重み (端 0.4 .. 中央 1.0)。
            vec2 d = uv - 0.5;
            float w = 1.0 - clamp(length(d) * 1.2, 0.0, 0.6);
            logSum += log(max(lum, 1e-5)) * w;
            weightSum += w;
        }
    }
    float target = exp(logSum / max(weightSum, 1e-5));
    target = clamp(target, min_lum, max_lum);

    // 有効値は texel (0,0) のみ (1x1 viewport 書込み)。 CLAMP_TO_EDGE
    // サンプラなので UV (0,0) が先頭 texel 中心に丸まる。
    float history = texture(historyLum, vec2(0.0)).r;
    // 初期化直後 (黒クリア = 0) は即時追従。
    float adapted = (history <= 0.0)
        ? target
        : mix(history, target, clamp(blend, 0.0, 1.0));

    outColor = vec4(adapted, adapted, adapted, 1.0);
}
