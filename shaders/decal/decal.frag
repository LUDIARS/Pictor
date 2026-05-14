// Pictor — Projected Decal
// シーン深度からワールド座標を復元し、 デカールの OBB 内かどうかを判定して
// ボックスのローカル UV でテクスチャをサンプルし、 シーンカラーへブレンド
// する。 fullscreen_quad.vert と組で使う。 ブレンドモード (Alpha/Additive/
// Multiply) は呼び出し側のパイプラインで切り替える。

#version 450

layout(location = 0) in  vec2 inUV;     // 0..1 (画面座標。 左上原点)
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform Scene {
    mat4 inv_view_proj;   // 深度 → ワールド座標 復元用
};
layout(set = 0, binding = 1) uniform sampler2D depthTex;   // シーン深度
layout(set = 1, binding = 0) uniform sampler2D decalTex;   // デカール画像

layout(push_constant) uniform PC {
    mat4  inv_decal;      // inverse(decal world transform)
    float opacity;
    float angle_fade;     // 投影面の角度フェード閾値 (0 で無効)
    float depth_fade;     // OBB Y 端のフェード幅
    float _pad0;
    vec4  decal_axis;     // xyz = デカールの投影軸 (ワールド空間, 正規化済み)
};

void main() {
    float depth = texture(depthTex, inUV).r;   // Vulkan depth [0,1]
    if (depth >= 1.0) discard;                 // 背景 = サーフェス無し

    // 画面 UV + depth → NDC → ワールド座標
    vec2 ndc = inUV * 2.0 - 1.0;               // Vulkan: y は下向き
    vec4 clip = vec4(ndc, depth, 1.0);
    vec4 worldH = inv_view_proj * clip;
    vec3 world  = worldH.xyz / worldH.w;

    // デカールローカル空間へ。 OBB は [-0.5, 0.5]^3。
    vec3 local = (inv_decal * vec4(world, 1.0)).xyz;
    vec3 a = abs(local);
    if (a.x > 0.5 || a.y > 0.5 || a.z > 0.5) discard;

    // ローカル XZ をデカール UV に。
    vec2 uv = local.xz + 0.5;
    vec4 d = texture(decalTex, uv);

    // OBB の Y 端でフェード (depth_fade 幅)。
    float dfade = 1.0 - smoothstep(0.5 - max(depth_fade, 1e-4), 0.5, a.y);

    // 投影面の角度フェード: サーフェス法線を深度の微分から再構成し、
    // デカール投影軸との一致度が低い (= 横向きの面) ほどフェードする。
    float afade = 1.0;
    if (angle_fade > 0.0) {
        vec3 dWx = dFdx(world);
        vec3 dWy = dFdy(world);
        vec3 surf_n = normalize(cross(dWx, dWy));
        float facing = abs(dot(surf_n, normalize(decal_axis.xyz)));
        afade = smoothstep(0.0, angle_fade, facing);
    }

    float alpha = d.a * opacity * dfade * afade;
    if (alpha <= 0.0) discard;
    outColor = vec4(d.rgb, alpha);   // ブレンドはパイプライン側
}
