#version 450

/// 影絵デモ — 切り絵シート用 depth-only pass 頂点シェーダ。
/// sp_depth.vert との違いは、カットアウト判定のために uv を
/// フラグメントへ渡すことだけ。

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec2 out_uv;

layout(push_constant) uniform Push {
    mat4 model;
    mat4 light_view_proj;
} pc;

void main() {
    out_uv = in_uv;
    gl_Position = pc.light_view_proj * pc.model * vec4(in_pos, 1.0);
}
