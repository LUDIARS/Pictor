// Pictor — SHaRC 拡張: present (SSBO 直読みトーンマップ)
//
// resolve の出力 (HDR 放射輝度 SSBO) をフラグメントから直接読んで
// Reinhard + gamma 2.2 でスワップチェインへ描く。 CPU への readback /
// テクスチャ再アップロードを行わない全 GPU 経路の終端。

#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(std430, set = 0, binding = 0) readonly buffer SharcOutput {
    vec4 gpu_sharc_output[];   // rgb = HDR 放射輝度, a = キャッシュヒット率
};

layout(push_constant) uniform PresentParams {
    uint  renderWidth;
    uint  renderHeight;
    float exposure;
    float pad0;
};

vec3 tonemap(vec3 hdr) {
    vec3 v = max(hdr * exposure, vec3(0.0));
    v = v / (1.0 + v);
    return pow(v, vec3(1.0 / 2.2));
}

void main() {
    // Vulkan の gl_FragCoord は左上原点 — レンダバッファも行 0 = 上で一致
    uint px = min(uint(inUv.x * float(renderWidth)), renderWidth - 1u);
    uint py = min(uint(inUv.y * float(renderHeight)), renderHeight - 1u);
    vec3 hdr = gpu_sharc_output[py * renderWidth + px].rgb;
    outColor = vec4(tonemap(hdr), 1.0);
}
