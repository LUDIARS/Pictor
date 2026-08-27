#version 450

/// 影絵デモ — 舞台裏ビュー (B キー) 用のオブジェクトフラグメントシェーダ。
/// 8 灯のセロファン光を Lambert で受けるだけの確認用 (影判定なし)。

layout(location = 0) in vec3 in_world_pos;
layout(location = 1) in vec3 in_normal;

layout(location = 0) out vec4 out_color;

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

        float cone = 1.0;
        if (light.pos_type.w < 0.5) {
            float cos_dir = dot(-L, normalize(light.dir_cone.xyz));
            cone = smoothstep(light.dir_cone.w, light.dir_cone.w + 0.025, cos_dir);
        }

        accum += light.color_params.rgb * atten * cone * max(dot(N, L), 0.0);
    }

    vec3 color = pc.tint.rgb * accum;
    color = clamp((color * (2.51 * color + 0.03)) /
                  (color * (2.43 * color + 0.59) + 0.14), 0.0, 1.0);
    out_color = vec4(color, 1.0);
}
