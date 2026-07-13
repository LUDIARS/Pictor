// Pictor — Camera Motion Blur (再投影方式) fullscreen pass
// 深度から現フレームの NDC 位置を再構築し、 reproj = prevVP * inverse(currVP)
// で前フレームの NDC へ再投影。 その差分 (スクリーン速度) に沿って
// シーンカラーをサンプルする。 per-object velocity は将来 (phase 3)。
//
// binding 0 = シーンカラー (HDR)、 binding 1 = シーン深度 (__depth__)。
// valid = 0 (初回フレーム / カメラワープ / 無効) のとき素通し (恒等縮退)。
//
// 行列レイアウト: pictor::float4x4 (m[row][col] 平坦化) をそのまま受ける。
// transform_point 相当の掛け順 (v * M) で評価する。

#version 450

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;

layout(push_constant) uniform MotionBlurPC {
    mat4  reproj;         // prevVP * inverse(currVP)  (float4x4::m と同一並び)
    float intensity;      // ブラー長スケール
    float max_velocity;   // NDC 単位の速度クランプ
    uint  sample_count;   // 上限 16
    uint  valid;          // 0 = 素通し
};

void main() {
    vec3 center = texture(sceneColor, inUV).rgb;

    if (valid == 0u || intensity <= 0.0) {
        outColor = vec4(center, 1.0);
        return;
    }

    float depth = texture(sceneDepth, inUV).r;

    // 現フレーム NDC (Vulkan: z in [0,1]、 y は下向き +)。
    vec4 ndc = vec4(inUV * 2.0 - 1.0, depth, 1.0);

    // float4x4 は row-vector 前提 (transform_point と同じ v * M)。
    vec4 prev = ndc * reproj;
    prev.xyz /= max(abs(prev.w), 1e-6) * sign(prev.w);

    // スクリーン速度 (UV 単位)。 NDC 差分の半分が UV 差分。
    vec2 velocity = (ndc.xy - prev.xy) * 0.5 * intensity;

    float speed = length(velocity);
    if (speed < 1e-5) {
        outColor = vec4(center, 1.0);
        return;
    }
    if (speed > max_velocity) {
        velocity *= max_velocity / speed;
    }

    // 速度ベクトルに沿って中心を挟んで前後対称にサンプル。
    uint samples = clamp(sample_count, 2u, 16u);
    vec3  sum = center;
    float weightSum = 1.0;
    for (uint i = 1u; i < samples; ++i) {
        float t = float(i) / float(samples - 1u) - 0.5;   // -0.5 .. +0.5
        vec2 uv = inUV + velocity * t;
        // 画面外サンプルはエッジ引き伸ばしを避けるため重みを落とす。
        float inBounds = (uv == clamp(uv, vec2(0.0), vec2(1.0))) ? 1.0 : 0.0;
        // 中心寄りを重くする三角重み。
        float w = (1.0 - abs(t) * 2.0 * 0.5) * inBounds;
        sum += texture(sceneColor, clamp(uv, vec2(0.0), vec2(1.0))).rgb * w;
        weightSum += w;
    }

    outColor = vec4(sum / weightSum, 1.0);
}
