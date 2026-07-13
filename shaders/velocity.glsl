// Pictor — velocity buffer 書込みヘルパー (ホストのシーンシェーダ用 include)
// scene pass MRT 化 (PostProcessConfig::velocity.enabled) 時、 フラグメント
// シェーダの location 1 へスクリーン速度 (UV 差分) を書く。
//
// 頂点シェーダで現/前フレームの clip 座標を出力し、 フラグメントで:
//   layout(location = 1) out vec2 outVelocity;
//   outVelocity = pictor_encode_velocity(clipNow, clipPrev);
//
// ジッタ付き投影 (TAA) の場合は「ジッタ無しの」行列で clip を計算すること。

#ifndef PICTOR_VELOCITY_GLSL
#define PICTOR_VELOCITY_GLSL

// clip 座標対 → スクリーン速度 (UV 単位、 現在 - 前フレーム)。
vec2 pictor_encode_velocity(vec4 clip_now, vec4 clip_prev) {
    vec2 ndc_now  = clip_now.xy  / max(abs(clip_now.w),  1e-6) * sign(clip_now.w);
    vec2 ndc_prev = clip_prev.xy / max(abs(clip_prev.w), 1e-6) * sign(clip_prev.w);
    return (ndc_now - ndc_prev) * 0.5;
}

#endif // PICTOR_VELOCITY_GLSL
