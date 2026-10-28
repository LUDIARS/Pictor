// Pictor — Projected Decal (組み込みマテリアル)
// シーン深度からワールド座標を復元し、 デカールの OBB 内かどうかを判定して
// ボックスのローカル UV でテクスチャをサンプルし、 シーンカラーへブレンド
// する。 fullscreen_quad.vert と組で使う。 ブレンドモード (Alpha/Additive/
// Multiply) は呼び出し側のパイプラインで切り替える。 独自マテリアルは
// decal_common.glsl を include して同じ契約で書く。

#version 450
#extension GL_GOOGLE_include_directive : require

#include "decal_common.glsl"

void main() {
    DecalSample s;
    if (!decal_sample(s)) discard;

    vec4 d = texture(decalTex, s.uv);
    float alpha = d.a * opacity * s.fade;
    if (alpha <= 0.0) discard;
    outColor = vec4(d.rgb, alpha);   // ブレンドはパイプライン側
}
