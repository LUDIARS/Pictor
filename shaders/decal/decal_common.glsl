// Pictor — Projected Decal の共通インタフェース
// 組み込み decal.frag と、 DecalSystem::register_material() で登録する
// 独自マテリアルが同じ set/binding と push constant 配置を共有するための
// include。 fullscreen_quad.vert と組で使う。
//
// 使い方 (マテリアル側):
//   #version 450
//   #extension GL_GOOGLE_include_directive : require
//   #include "decal_common.glsl"
//   void main() {
//       DecalSample s;
//       if (!decal_sample(s)) discard;
//       outColor = vec4(..., s.fade * opacity);
//   }

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
    vec4  params[2];      // マテリアル固有 (DecalDescriptor::params)
};

struct DecalSample {
    vec3  world;    // 復元したワールド座標
    vec3  local;    // デカールローカル ([-0.5, 0.5]^3)
    vec2  uv;       // ローカル XZ → 0..1
    vec3  normal;   // 深度の微分から再構成したサーフェス法線
    float fade;     // depth fade * angle fade (opacity は含まない)
};

// 画素がデカールの OBB 内なら s を埋めて true。 背景・OBB 外は false。
bool decal_sample(out DecalSample s) {
    s.world = vec3(0.0); s.local = vec3(0.0); s.uv = vec2(0.0);
    s.normal = vec3(0.0, 1.0, 0.0); s.fade = 0.0;

    float depth = texture(depthTex, inUV).r;   // Vulkan depth [0,1]
    if (depth >= 1.0) return false;            // 背景 = サーフェス無し

    // 画面 UV + depth → NDC → ワールド座標
    vec2 ndc = inUV * 2.0 - 1.0;               // Vulkan: y は下向き
    vec4 worldH = inv_view_proj * vec4(ndc, depth, 1.0);
    s.world = worldH.xyz / worldH.w;

    // デカールローカル空間へ。 OBB は [-0.5, 0.5]^3。
    s.local = (inv_decal * vec4(s.world, 1.0)).xyz;
    vec3 a = abs(s.local);
    if (a.x > 0.5 || a.y > 0.5 || a.z > 0.5) return false;

    s.uv = s.local.xz + 0.5;

    // OBB の Y 端でフェード (depth_fade 幅)。
    float dfade = 1.0 - smoothstep(0.5 - max(depth_fade, 1e-4), 0.5, a.y);

    // 投影面の角度フェード: 投影軸との一致度が低い (= 横向きの面) ほどフェード。
    s.normal = normalize(cross(dFdx(s.world), dFdy(s.world)));
    float afade = 1.0;
    if (angle_fade > 0.0) {
        float facing = abs(dot(s.normal, normalize(decal_axis.xyz)));
        afade = smoothstep(0.0, angle_fade, facing);
    }
    s.fade = dfade * afade;
    return true;
}
