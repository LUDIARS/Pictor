#version 450

/// 影絵デモ — depth-only pass (per-light shadow map).
/// ライト 1 灯ぶんの view-projection で occluder (球・立方体) を描き、
/// スクリーン (障子) 側のハードシャドウ判定に使う深度だけを残す。

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(push_constant) uniform Push {
    mat4 model;
    mat4 light_view_proj;
} pc;

void main() {
    gl_Position = pc.light_view_proj * pc.model * vec4(in_pos, 1.0);
}
