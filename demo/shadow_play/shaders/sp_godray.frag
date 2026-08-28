#version 450
#extension GL_GOOGLE_include_directive : require

/// 影絵デモ — 月光ゴッドレイ (観客側の前方体積レイマーチ)。
///
/// 月光は「切り絵シートの穴 → (型の遮蔽) → 障子紙」を通ったあと、
/// 観客席側 (z > 0) へ光条として続く — という影絵の演出。物理的には
/// 紙の拡散で失われる指向性だが、月見窓の光芒として意図的に残す。
///
/// 各サンプル点の可視判定は月ライトの shadow map 1 枚 (シートの
/// カットアウト + 型の影が焼き込まれている) の step 比較のみ。
/// これにより「シートで遮蔽された月光は前方体積にも一切現れない」。
///
/// 加算合成 (ONE/ONE) で障子スクリーンの上へ重ねる。

#include "sp_scene.glsl"
#include "sp_cutout.glsl"

layout(set = 0, binding = 1) uniform sampler2DArray shadow_atlas;
layout(set = 0, binding = 3) uniform sampler2D kirie_hawk; // 鷹の切り絵 (月の中)

layout(location = 0) in vec2 in_ndc;

layout(location = 0) out vec4 out_color;

// Jitter と dt 正規化で明るさを保ちつつ、全画面パスの負荷を抑える。
const int kGodraySteps = 24;

// 障子スクリーン実寸 (main.cpp kScreenW/kScreenH)。光条を紙の矩形 +
// 余白へ柔らかく閉じ込めるためのフェザー判定に使う。
const vec2 kScreenHalf = vec2(4.0, 2.25);
const float kScreenFeather = 0.45;
// Keep in sync with sp_screen.frag: water reflects screen light but has no ray volume.
const float SP_WATERLINE = -1.35;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    if (scene.moon_pos.w < 0.5) {
        out_color = vec4(0.0);
        return;
    }

    // カメラ基底からピクセルのワールドレイを再構成。
    // proj は Vulkan Y-flip (proj[1][1] < 0) なので NDC y は符号反転で戻す。
    vec3 ray_dir = normalize(scene.cam_fwd.xyz
                             + in_ndc.x * scene.cam_right.w * scene.cam_right.xyz
                             - in_ndc.y * scene.cam_up.w * scene.cam_up.xyz);
    vec3 ray_origin = scene.camera_pos.xyz;

    // 前方体積: 障子スクリーン (z=0) から観客側 z=front までのスラブ。
    float front = scene.godray_params.z;
    if (abs(ray_dir.z) < 1e-4) {
        out_color = vec4(0.0);
        return;
    }
    float t_a = (front - ray_origin.z) / ray_dir.z;
    float t_b = (0.0   - ray_origin.z) / ray_dir.z;
    float t_near = max(min(t_a, t_b), 0.0);
    float t_far  = max(t_a, t_b);
    if (t_far <= t_near) {
        out_color = vec4(0.0);
        return;
    }

    float dt = (t_far - t_near) / float(kGodraySteps);
    float jitter = hash12(gl_FragCoord.xy); // バンディング抑制のディザ

    float density    = scene.godray_params.x;
    float extinction = scene.godray_params.y;

    vec3 accum = vec3(0.0);
    for (int i = 0; i < kGodraySteps; ++i) {
        float t = t_near + (float(i) + jitter) * dt;
        vec3 p = ray_origin + ray_dir * t;
        if (p.y < SP_WATERLINE) continue;

        // 月光可視判定 (フラスタム外 = 月光なし)。人影は解析判定で遮る。
        if (sp_figure_blocks(p, scene.moon_pos.xyz)) continue;
        bool in_frustum;
        float vis = sp_hard_shadow_vp(scene.moon_view_proj, SP_MOON_LAYER,
                                      p, shadow_atlas, in_frustum);
        if (!in_frustum || vis <= 0.0) continue;

        // 紙を透過してきた光なので、紙の矩形 (+ 余白) の外は柔らかく落とす
        vec2 edge = abs(p.xy) - kScreenHalf;
        float box = (1.0 - smoothstep(0.0, kScreenFeather, edge.x)) *
                    (1.0 - smoothstep(0.0, kScreenFeather, edge.y));
        if (box <= 0.0) continue;

        // 紙から離れるほど散逸 (前方への減衰)。鷹の切り絵は月の丸穴の
        // 光芒にも影を落とす (障子側の遮蔽と同じ sheet 座標判定)。
        vec2 sheet_p = sp_sheet_hit(p, scene.moon_pos.xyz);
        accum += sp_cutout_tint(sheet_p) * sp_hawk_filter(sheet_p, kirie_hawk) *
                 (box * exp(-extinction * p.z));
    }

    vec3 col = scene.moon_color.rgb * scene.moon_color.w
             * scene.godray_params.w   // 紙透過率
             * density * accum * dt;

    // 加算合成なのでソフトクリップして白飛びを防ぐ
    col = 1.0 - exp(-col);
    out_color = vec4(col, 1.0);
}
