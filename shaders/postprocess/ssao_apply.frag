// Pictor — SSAO (post-process 近似) fullscreen pass
// 深度バッファの近傍サンプルとの深度差から遮蔽を推定し、 シーンカラーへ
// 乗算する 1-pass 構成 (計算と適用を分離しない)。 ビュー空間再構築を
// 要しない深度差ベースの近似で、 プロジェクション行列に非依存 —
// カジュアル用途向け (spec/feature/postprocess-effects-design.md §2.1)。
// ライティング項別の正確な AO (間接光のみ減衰) は GI 側の責務。
//
// binding 0 = シーンカラー (HDR)、 binding 1 = シーン深度 (__depth__)。
// 無効化は intensity 0 (ao = 1 で恒等縮退)。

#version 450

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;

layout(push_constant) uniform SsaoPC {
    float radius_px;      // サンプル半径 (ピクセル)
    float bias;           // 無視する深度差の下限 (自己遮蔽防止)
    float range;          // 遮蔽とみなす深度差の上限 (depth 単位)
    float intensity;      // 遮蔽の効き 0..1 (0 = 恒等)
    float power;          // AO カーブ
    float texel_x;
    float texel_y;
    uint  sample_count;   // 上限 32
};

// golden-angle spiral kernel — 固定タップ列 (シェーダ内で乱数を使わず
// 決定的。 タップ数は sample_count で先頭から打ち切る)。
const float GOLDEN_ANGLE = 2.39996323;

void main() {
    vec3 color = texture(sceneColor, inUV).rgb;

    if (intensity <= 0.0) {
        outColor = vec4(color, 1.0);
        return;
    }

    float centerDepth = texture(sceneDepth, inUV).r;
    vec2  texel = vec2(texel_x, texel_y);
    uint  samples = min(sample_count, 32u);

    float occlusion = 0.0;
    for (uint i = 0u; i < samples; ++i) {
        // スパイラル配置: 半径は sqrt 分布で面に均一、 角度は黄金角。
        float t = (float(i) + 0.5) / float(samples);
        float angle = float(i) * GOLDEN_ANGLE;
        vec2 offset = vec2(cos(angle), sin(angle)) * sqrt(t) * radius_px;

        float sampleDepth = texture(sceneDepth, inUV + offset * texel).r;
        // 手前 (小さい深度) にあるサンプルが中心を遮る。
        float diff = centerDepth - sampleDepth;
        if (diff > bias) {
            // range を超える深度差は別物体の手前被り — 距離で減衰させる。
            float falloff = 1.0 - smoothstep(range * 0.5, range, diff);
            occlusion += falloff;
        }
    }
    occlusion /= float(samples);

    float ao = pow(clamp(1.0 - occlusion * intensity, 0.0, 1.0), power);
    outColor = vec4(color * ao, 1.0);
}
