/// 影絵デモ — SceneUBO 共有定義。
///
/// sp_* シェーダ全部がこの 1 ファイルを include する。SpLight / SceneUBO の
/// レイアウトは main.cpp の SpLightData / SpSceneUBO (std140 手動ミラー) と
/// 1:1 で一致させること。フィールドを増減したら C++ 側も必ず追従する。

struct SpLight {
    vec4 pos_type;      // xyz = 位置, w = 0:spot / 1:point
    vec4 dir_cone;      // xyz = 照射方向 (spot), w = cos(outer cone)
    vec4 color_params;  // rgb = セロファン色, w = 強度
    vec4 range_params;  // x = 影響半径 (0 = 無制限), yzw 予備
    mat4 view_proj;     // shadow map 用 light VP
};

layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 view;
    mat4 proj;
    mat4 view_proj;
    vec4 camera_pos;     // xyz + pad
    vec4 params;         // x = time, y = ambient, z = sss_strength, w = paper_sigma
    vec4 moon_pos;       // xyz = 月ライト位置, w = 有効フラグ (>0.5 で有効)
    vec4 moon_dir;       // xyz = 月の照射方向, w = 障子直接透過スケール
    vec4 moon_color;     // rgb = 月光色, w = 強度
    vec4 godray_params;  // x = 密度, y = 消散係数, z = 前方体積の奥行き, w = 紙透過率
    vec4 cam_right;      // xyz = カメラ右, w = tan(fovy/2) * aspect
    vec4 cam_up;         // xyz = カメラ上, w = tan(fovy/2)
    vec4 cam_fwd;        // xyz = カメラ前方, w 予備
    mat4 moon_view_proj; // 月ライトの shadow map 用 VP (layer = SP_MOON_LAYER)
    SpLight lights[8];
} scene;

// shadow_atlas の月ライト layer (0..7 = 通常 8 灯, 8 = 月)
const float SP_MOON_LAYER = 8.0;
const float SP_SHADOW_BIAS = 0.0018;

/// 任意の light VP + layer でのハードシャドウ判定。
/// 1 サンプル step 比較のみ (影絵の輪郭)。フラスタム外は「照射なし」扱いに
/// したい呼び出し側もあるため、フラスタム外を返り値で区別する:
///   返り値 1.0 = 照射 / 0.0 = 遮蔽。フラスタム外は out_in_frustum = false。
float sp_hard_shadow_vp(mat4 vp, float layer, vec3 world_pos,
                        sampler2DArray atlas, out bool out_in_frustum) {
    out_in_frustum = false;
    vec4 lp = vp * vec4(world_pos, 1.0);
    if (lp.w <= 0.0) return 0.0;
    vec3 ndc = lp.xyz / lp.w;
    vec2 suv = ndc.xy * 0.5 + 0.5;
    if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0 ||
        ndc.z < 0.0 || ndc.z > 1.0) return 0.0;
    out_in_frustum = true;
    float occluder = texture(atlas, vec3(suv, layer)).r;
    return step(ndc.z - SP_SHADOW_BIAS, occluder);
}

/// 8 灯ぶんの従来判定 (フラスタム外 = 照射扱い、既存挙動を維持)。
float sp_hard_shadow_light(int idx, vec3 world_pos, sampler2DArray atlas) {
    bool in_frustum;
    float s = sp_hard_shadow_vp(scene.lights[idx].view_proj, float(idx),
                                world_pos, atlas, in_frustum);
    return in_frustum ? s : 1.0;
}

/// 距離による影響半径の窓関数。range <= 0 は無制限 (1.0)。
/// (1 - (d/R)^4)^2 — 中心近傍はほぼ 1 のまま、R へ向けて滑らかに 0 へ。
/// 「光の重なり」は保ちながら、各灯のプール外周だけを絞る。
float sp_range_window(float dist, float range) {
    if (range <= 0.0) return 1.0;
    float q = dist / range;
    q *= q; q *= q;                    // (d/R)^4
    float w = clamp(1.0 - q, 0.0, 1.0);
    return w * w;
}
