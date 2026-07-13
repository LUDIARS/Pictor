// Pictor — SSGI: apply pass
// half-res の gather 結果 (pp_ssgi) を bilinear で引き伸ばし、 シーンへ
// 加算する。 intensity 0 で素通し (恒等縮退)。
//
// binding 0 = シーンカラー (HDR)、 binding 1 = SSGI (half-res)。

#version 450

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;
layout(set = 0, binding = 1) uniform sampler2D ssgiTex;

layout(push_constant) uniform SsgiApplyPC {
    float intensity;
    float _pad0;
    float _pad1;
    float _pad2;
};

void main() {
    vec3 scene = texture(sceneColor, inUV).rgb;
    if (intensity > 0.0) {
        vec3 gi = texture(ssgiTex, inUV).rgb;
        scene += gi * intensity;
    }
    outColor = vec4(scene, 1.0);
}
