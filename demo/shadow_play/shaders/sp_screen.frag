#version 450

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

layout(location = 0) in vec3 in_world_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

struct SpLight {
    vec4 pos_type;      // xyz = 位置, w = 0:spot / 1:point
    vec4 dir_cone;      // xyz = 照射方向 (spot), w = cos(outer cone)
    vec4 color_params;  // rgb = セロファン色, w = 強度
    mat4 view_proj;     // shadow map 用 light VP
};

layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 view;
    mat4 proj;
    mat4 view_proj;
    vec4 camera_pos;
    vec4 params;        // x = time, y = ambient, z = sss_strength, w = paper_sigma
    SpLight lights[8];
} scene;

// 8 灯ぶんの shadow map (sampled depth の 2D array、layer = light index)
layout(set = 0, binding = 1) uniform sampler2DArray shadow_atlas;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 tint;
} pc;

const float SHADOW_BIAS = 0.0018;

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

// ---- 1 灯ぶんの透過光 ----

/// ハードシャドウ判定。1 サンプル step 比較のみ (影絵の輪郭)。
float hard_shadow(int idx, vec3 world_pos) {
    vec4 lp = scene.lights[idx].view_proj * vec4(world_pos, 1.0);
    if (lp.w <= 0.0) return 1.0;
    vec3 ndc = lp.xyz / lp.w;
    vec2 suv = ndc.xy * 0.5 + 0.5;
    if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) return 1.0;
    float occluder = texture(shadow_atlas, vec3(suv, float(idx))).r;
    return step(ndc.z - SHADOW_BIAS, occluder);
}

void main() {
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

        vec3  to_light = light.pos_type.xyz - in_world_pos;
        float dist     = length(to_light);
        vec3  L        = to_light / dist;

        float atten = intensity / (1.0 + 0.045 * dist * dist);

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

        float shadow = hard_shadow(i, in_world_pos);

        // 直接透過 (ハードシャドウが乗る = 切り絵)
        accum += light.color_params.rgb * atten * cone * cos_in * transmit * shadow;
        // 紙内の多重散乱 (影判定なし = 影が完全黒に沈まない空気感)
        accum += light.color_params.rgb * atten * cone * sss_strength;
    }

    vec3 color = paper_base * accum;

    // 簡易 ACES トーンマップ
    color = clamp((color * (2.51 * color + 0.03)) /
                  (color * (2.43 * color + 0.59) + 0.14), 0.0, 1.0);

    // 周辺減光で舞台の暗がりへ落とす
    vec2 vig_uv = in_uv - 0.5;
    float vignette = 1.0 - 0.55 * dot(vig_uv, vig_uv) * 2.2;
    color *= clamp(vignette, 0.0, 1.0);

    out_color = vec4(color, 1.0);
}
