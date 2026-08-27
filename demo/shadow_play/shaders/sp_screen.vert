#version 450
#extension GL_GOOGLE_include_directive : require

/// 影絵デモ — スクリーン (障子紙) 頂点シェーダ。
/// world 座標をそのままフラグメントへ渡し、8 灯 + 月の影判定は frag 側で行う。

#include "sp_scene.glsl"

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec3 out_world_pos;
layout(location = 1) out vec3 out_normal;
layout(location = 2) out vec2 out_uv;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 tint;          // rgb = ベース色係数, w 未使用
} pc;

void main() {
    vec4 world = pc.model * vec4(in_pos, 1.0);
    out_world_pos = world.xyz;
    out_normal    = normalize(mat3(pc.model) * in_normal);
    out_uv        = in_uv;
    gl_Position   = scene.view_proj * world;
}
