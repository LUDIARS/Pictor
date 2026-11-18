#version 450
#extension GL_GOOGLE_include_directive : require
// 草のピクセルシェーダー: 不透明 (カットアウトなし)。
//   UV0.y で根元 → 先端の色グラデーションと AO、裏から光が透ける「透過光」
#include "common.glsl"
#include "grass_common.glsl"

layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv0;
layout(location = 3) in vec3 v_tint;
layout(location = 0) out vec4 o_color;

void main() {
    vec3 n = normalize(v_normal);
    if (!gl_FrontFacing) n = -n;
    vec3 V = normalize(F.cam_pos.xyz - v_world);
    vec3 L = F.sun_dir.xyz;
    vec3 sun = g_env[0].rgb, sky = g_env[1].rgb, hor = g_env[2].rgb;

    float h = v_uv0.y;
    vec3 root_col = v_tint.y > 0.5 ? vec3(0.10, 0.22, 0.05) : vec3(0.06, 0.18, 0.04);
    vec3 tip_col  = v_tint.y > 0.5 ? vec3(0.55, 0.62, 0.20) : vec3(0.30, 0.60, 0.12);
    vec3 albedo = mix(root_col, tip_col, h) * mix(0.8, 1.2, v_tint.x);

    // デバッグ: セル (= インスタンス番号の塊) ごとに色分け
    if (flag(1)) albedo = 0.3 + 0.7 * hash33(vec3(floor(v_tint.z / 512.0)));

    float ao = mix(1.0 - F.p[2].w, 1.0, h);                      // 根元ほど暗い
    float wrap = max((dot(n, L) + 0.4) / 1.4, 0.0);
    float trans = pow(max(dot(-V, L), 0.0), 4.0) * h * 0.8;      // 透過光 (逆光で先端が光る)
    vec3 col = albedo * (sun * (wrap + trans) + mix(hor, sky, 0.5 + 0.5 * n.y) * 0.6) * ao;
    col += sun * pow(max(dot(reflect(-L, n), V), 0.0), 24.0) * 0.08 * h;

    float fog = 1.0 - exp(-length(F.cam_pos.xyz - v_world) * 0.004);
    col = mix(col, hor * 0.7, fog);
    o_color = finalize(col);
}
