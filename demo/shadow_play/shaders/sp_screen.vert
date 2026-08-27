#version 450

/// 影絵デモ — スクリーン (障子紙) 頂点シェーダ。
/// world 座標をそのままフラグメントへ渡し、8 灯ぶんの影判定は frag 側で行う。

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec3 out_world_pos;
layout(location = 1) out vec3 out_normal;
layout(location = 2) out vec2 out_uv;

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
    vec4 camera_pos;    // xyz + pad
    vec4 params;        // x = time, y = ambient, z = sss_strength, w = paper_sigma
    SpLight lights[8];
} scene;

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
