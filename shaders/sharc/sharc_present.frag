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

// Bloom チェーンの up[0] (半解像度、 露出スケール済み)。 bilinear で
// フル解像度へ引き伸ばして「トーンマップ前」 に加算合成する
layout(set = 0, binding = 1) uniform sampler2D bloomTex;

layout(push_constant) uniform PresentParams {
    uint  renderWidth;
    uint  renderHeight;
    float exposure;
    float albedoMode;      // > 0.5 = アルベド素通し (gamma のみ、 露出なし)
    float bloomStrength;   // 0 = bloom 無効
};

// 出力はリニアのまま返す — swapchain は B8G8R8A8_SRGB 優先
// (vulkan_context.cpp:596) で attachment 側が sRGB エンコードするため、
// 手動 pow(1/2.2) を掛けると二重ガンマで白く washed になる (P1-10)。
vec3 tonemap(vec3 hdr, vec3 bloom) {
    if (albedoMode > 0.5) {
        return max(hdr, vec3(0.0));
    }
    // 輝度ベース Reinhard: 明るさのみ圧縮し色相・彩度 (テクスチャ色) を
    // 保存する。 チャネル別 Reinhard は高輝度で白へ収束し色が消える。
    // bloom は露出スケール済みなので露出後・トーンマップ前に加算。
    vec3 v = max(hdr * exposure, vec3(0.0)) + bloom * bloomStrength;
    float luma = dot(v, vec3(0.2126, 0.7152, 0.0722));
    float scale = (luma > 1e-6) ? (luma / (1.0 + luma)) / luma : 0.0;
    return clamp(v * scale, 0.0, 1.0);
}

void main() {
    // Vulkan の gl_FragCoord は左上原点 — レンダバッファも行 0 = 上で一致
    uint px = min(uint(inUv.x * float(renderWidth)), renderWidth - 1u);
    uint py = min(uint(inUv.y * float(renderHeight)), renderHeight - 1u);
    vec3 hdr = gpu_sharc_output[py * renderWidth + px].rgb;
    vec3 bloom = (bloomStrength > 0.0) ? texture(bloomTex, inUv).rgb
                                       : vec3(0.0);
    outColor = vec4(tonemap(hdr, bloom), 1.0);
}
