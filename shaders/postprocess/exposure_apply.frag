// Pictor — Auto Exposure: 適用 pass
// exposure_measure が出した適応輝度から露出係数 (key / avg_lum) を計算し、
// シーンカラーへ乗算する。 bloom 抽出より前に走るため、 露出済みの値で
// threshold が効く。 key <= 0 で素通し (恒等縮退)。
//
// binding 0 = シーンカラー (HDR)、 binding 1 = 適応輝度 (pp_exposure、
// 有効値は texel (0,0) のみ — CLAMP_TO_EDGE で UV (0,0) 読み)。

#version 450

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;
layout(set = 0, binding = 1) uniform sampler2D adaptedLum;

layout(push_constant) uniform ExposureApplyPC {
    float key;        // 中間グレー目標 (既定 0.18)。 <= 0 で素通し
    float _pad0;
    float _pad1;
    float _pad2;
};

void main() {
    vec3 color = texture(sceneColor, inUV).rgb;

    if (key > 0.0) {
        float avgLum = texture(adaptedLum, vec2(0.0)).r;
        float exposure = key / max(avgLum, 1e-5);
        color *= exposure;
    }

    outColor = vec4(color, 1.0);
}
