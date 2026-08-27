#version 450

/// 影絵デモ — 舞台裏ビュー (B キー) 用のオブジェクト頂点シェーダ。
/// ライト配置確認のための簡易 lit 描画。

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec3 out_world_pos;
layout(location = 1) out vec3 out_normal;

struct SpLight {
    vec4 pos_type;
    vec4 dir_cone;
    vec4 color_params;
    mat4 view_proj;
};

layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 view;
    mat4 proj;
    mat4 view_proj;
    vec4 camera_pos;
    vec4 params;
    SpLight lights[8];
} scene;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 tint;
} pc;

void main() {
    vec4 world = pc.model * vec4(in_pos, 1.0);
    out_world_pos = world.xyz;
    out_normal    = normalize(mat3(pc.model) * in_normal);
    gl_Position   = scene.view_proj * world;
}
