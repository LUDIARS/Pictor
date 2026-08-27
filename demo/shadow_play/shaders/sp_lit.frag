#version 450
#extension GL_GOOGLE_include_directive : require

/// 影絵デモ — 舞台裏ビュー (B キー) 用のオブジェクトフラグメントシェーダ。
/// 8 灯のセロファン光を Lambert で受ける確認用 (8 灯は影判定なし)。
/// 月光のみ shadow map (layer 8) を引く — 切り絵シートのカットアウトが
/// 型の上へ直接投影されるのを舞台裏から確認できる。

#include "sp_scene.glsl"

layout(location = 0) in vec3 in_world_pos;
layout(location = 1) in vec3 in_normal;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 1) uniform sampler2DArray shadow_atlas;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 tint;
} pc;

void main() {
    vec3 N = normalize(in_normal);
    vec3 accum = vec3(scene.params.y);

    for (int i = 0; i < 8; ++i) {
        SpLight light = scene.lights[i];
        float intensity = light.color_params.w;
        if (intensity <= 0.0) continue;

        vec3  to_light = light.pos_type.xyz - in_world_pos;
        float dist     = length(to_light);
        vec3  L        = to_light / dist;

        float atten = intensity / (1.0 + 0.045 * dist * dist);
        // 影響半径 (観客ビューと同じ絞り)
        atten *= sp_range_window(dist, light.range_params.x);

        float cone = 1.0;
        if (light.pos_type.w < 0.5) {
            float cos_dir = dot(-L, normalize(light.dir_cone.xyz));
            cone = smoothstep(light.dir_cone.w, light.dir_cone.w + 0.025, cos_dir);
        }

        accum += light.color_params.rgb * atten * cone * max(dot(N, L), 0.0);
    }

    // 月光 (切り絵シート越し、ハードシャドウ)
    if (scene.moon_pos.w > 0.5) {
        vec3  to_moon = scene.moon_pos.xyz - in_world_pos;
        vec3  Lm      = normalize(to_moon);
        bool in_frustum;
        float shadow = sp_hard_shadow_vp(scene.moon_view_proj, SP_MOON_LAYER,
                                         in_world_pos, shadow_atlas, in_frustum);
        if (!in_frustum) shadow = 0.0;
        accum += scene.moon_color.rgb * scene.moon_color.w * shadow *
                 max(dot(N, Lm), 0.0);
    }

    vec3 color = pc.tint.rgb * accum;
    color = clamp((color * (2.51 * color + 0.03)) /
                  (color * (2.43 * color + 0.59) + 0.14), 0.0, 1.0);
    out_color = vec4(color, 1.0);
}
