#version 450
#extension GL_GOOGLE_include_directive : require

/// 影絵デモ — スクリーン (障子紙) フラグメントシェーダ。
///
/// 表現の柱:
///   1. ハードシャドウ — shadow map を 1 サンプル step 比較のみ (PCF なし)。
///      切り絵 / 影絵のパキッとした輪郭を再現する。
///   2. セロファン — 各灯に彩度の高いゲル色。スポットのコーン縁も
///      狭い smoothstep でくっきり落とす。
///   3. 紙の透過 (SSS 近似) — 入射角依存の exp 減衰 (斜め入射ほど紙中の
///      光路が伸びる) + 多重散乱項。散乱項は影判定を掛けないので、影の中も
///      完全な黒に沈まず紙が仄かに光る = 空気感。
///   4. 影響半径 (range) — 各灯のプールを (1-(d/R)^4)^2 の窓で絞り、
///      重なりの中心は保ったまま外周の洗いを限定する。
///   5. 月光 — 切り絵シート (layer 8 の shadow map に焼き込み) の
///      カットアウトを通った月光だけが紙に透ける。

#include "sp_scene.glsl"
#include "sp_cutout.glsl"

layout(location = 0) in vec3 in_world_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 1) uniform sampler2DArray shadow_atlas;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 tint;
} pc;

// Keep in sync with sp_godray.frag: reflection starts at this world-space y.
const float SP_WATERLINE = -1.35;

// ---- 和紙の繊維ノイズ (手続き) ----

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float value_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash12(i);
    float b = hash12(i + vec2(1.0, 0.0));
    float c = hash12(i + vec2(0.0, 1.0));
    float d = hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

/// 和紙: 低周波のムラ + 横に走る繊維筋
float paper_fiber(vec2 uv) {
    float blotch = value_noise(uv * 14.0) * 0.5 + value_noise(uv * 47.0) * 0.3;
    float strand = value_noise(vec2(uv.x * 180.0, uv.y * 9.0)) * 0.2;
    return 0.82 + 0.18 * (blotch + strand);
}

void main() {
    bool is_water = in_world_pos.y < SP_WATERLINE;
    vec3 shaded_pos = in_world_pos;
    if (is_water) {
        float wave = 0.10 * (value_noise(vec2(in_world_pos.x * 2.7,
                                               scene.params.x * 0.5)) - 0.5)
                   + 0.04 * sin(in_world_pos.x * 9.0 + scene.params.x * 0.8);
        shaded_pos.y = 2.0 * SP_WATERLINE - in_world_pos.y + wave;
    }
    // 紙の裏面 (-Z 側) から光が入る。表面法線は +Z。
    vec3 back_normal = -normalize(in_normal);

    float fiber = paper_fiber(in_uv);
    vec3 paper_base = vec3(1.0, 0.97, 0.90) * pc.tint.rgb * fiber;

    float ambient      = scene.params.y;
    float sss_strength = scene.params.z;
    float paper_sigma  = scene.params.w;

    vec3 accum = vec3(ambient);

    for (int i = 0; i < 8; ++i) {
        SpLight light = scene.lights[i];
        float intensity = light.color_params.w;
        if (intensity <= 0.0) continue;

        vec3  to_light = light.pos_type.xyz - shaded_pos;
        float dist     = length(to_light);
        vec3  L        = to_light / dist;

        float atten = intensity / (1.0 + 0.045 * dist * dist);
        // 影響半径: 各灯のプール外周を絞る (中心の重なりは保つ)
        atten *= sp_range_window(dist, light.range_params.x);

        // スポットのコーン。縁は狭い smoothstep でセロファンの円をくっきり出す。
        float cone = 1.0;
        if (light.pos_type.w < 0.5) {
            float cos_dir = dot(-L, normalize(light.dir_cone.xyz));
            cone = smoothstep(light.dir_cone.w, light.dir_cone.w + 0.025, cos_dir);
        }

        // 裏面入射角。真後ろからの光ほど良く透ける。
        float cos_in = max(dot(L, back_normal), 0.0);

        // 紙中の光路長は 1/cosθ で伸びる → 斜め入射は減衰が強い。
        float transmit = exp(-paper_sigma / max(cos_in, 0.05));

        float shadow = sp_hard_shadow_light(i, shaded_pos, shadow_atlas);

        // 直接透過 (ハードシャドウが乗る = 切り絵)
        accum += light.color_params.rgb * atten * cone * cos_in * transmit * shadow;
        // 紙内の多重散乱 (影判定なし = 影が完全黒に沈まない空気感)
        accum += light.color_params.rgb * atten * cone * sss_strength;
    }

    // ---- 月光 (切り絵シート越し) ----
    // シートのカットアウトと型の影は月ライトの shadow map (layer 8) に
    // 焼き込まれている。シートに遮られた月光はここへ一切届かない。
    if (scene.moon_pos.w > 0.5) {
        vec3  to_moon = scene.moon_pos.xyz - shaded_pos;
        float mdist   = length(to_moon);
        vec3  Lm      = to_moon / mdist;

        float cos_in   = max(dot(Lm, back_normal), 0.0);
        float transmit = exp(-paper_sigma / max(cos_in, 0.05));

        bool in_frustum;
        float shadow = sp_hard_shadow_vp(scene.moon_view_proj, SP_MOON_LAYER,
                                         shaded_pos, shadow_atlas, in_frustum);
        if (!in_frustum) shadow = 0.0; // フラスタム外に月光はない
        // 人影は解析判定で月光を遮る (水面反射側にも同じ影が出る)
        if (shadow > 0.0 && sp_figure_blocks(shaded_pos, scene.moon_pos.xyz))
            shadow = 0.0;

        float inten = scene.moon_color.w;
        vec3 moon_tint = sp_cutout_tint(sp_sheet_hit(shaded_pos, scene.moon_pos.xyz));
        accum += scene.moon_color.rgb * moon_tint * inten * scene.moon_dir.w * cos_in * transmit * shadow;
        // 月光の多重散乱も、シートの穴を通って紙へ届いた範囲だけに生じる。
        accum += scene.moon_color.rgb * moon_tint * inten * sss_strength * 0.25 * shadow;
    }

    if (is_water) {
        accum *= 0.55;
        accum *= vec3(0.80, 0.90, 1.05);
        if (in_world_pos.y > SP_WATERLINE - 0.08) accum += vec3(0.06);
    }

    vec3 color = paper_base * accum;

    // 簡易 ACES トーンマップ
    color = clamp((color * (2.51 * color + 0.03)) /
                  (color * (2.43 * color + 0.59) + 0.14), 0.0, 1.0);

    // 周辺減光で舞台の暗がりへ落とす
    vec2 vig_uv = in_uv - 0.5;
    float vignette = 1.0 - 0.80 * dot(vig_uv, vig_uv) * 2.2;
    color *= clamp(vignette, 0.0, 1.0);

    out_color = vec4(color, 1.0);
}
