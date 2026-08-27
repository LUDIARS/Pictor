#version 450
#extension GL_GOOGLE_include_directive : require

/// 影絵デモ — 舞台裏ビューでの切り絵シート可視化 (フラグメント)。
/// 遮蔽の実体は shadow map (sp_depth_cut) 側なので、ここは配置と
/// カットアウト形状の目視確認用。穴は discard し、紙面は暗い藍紙 +
/// 月光側の縁だけ仄かに明るくする。

#include "sp_cutout.glsl"
#include "sp_scene.glsl"

layout(location = 0) in vec3 in_world_pos;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 tint;
} pc;

void main() {
    if (sp_cutout_hole(in_uv)) discard;

    // 暗い藍紙。月が有効なら月光の色を僅かに縁へ滲ませる (確認用の演出)。
    vec3 paper = vec3(0.045, 0.050, 0.085) * pc.tint.rgb;
    if (scene.moon_pos.w > 0.5) {
        paper += scene.moon_color.rgb * 0.020;
    }
    out_color = vec4(paper, 1.0);
}
