// Pictor — SHaRC ハイブリッド経路: G-buffer フラグメントシェーダ
//
// RT0 rgba16f = albedo.rgb + ベイク AO
// RT1 rgba16f = 法線.xyz + roughness
// RT2 rg32f   = カメラ距離 (0 = ミス/空) + SSS MFP
//
// アルベドはレイ版 hit パスの footprint LOD と違い、 ラスタの暗黙微分で
// texture() の自動 LOD が使える (品質はこちらが上)。

#version 450

struct SharcMaterial {
    vec4 albedoRough;  // rgb = albedo, a = roughness
    vec4 mfpPad;       // x = SSS MFP, y = atlas layer+1 (0 = テクスチャなし)
};

layout(std430, set = 0, binding = 3) readonly buffer GbMaterials {
    SharcMaterial gb_materials[];
};
layout(set = 0, binding = 4) uniform sampler2DArray gbAlbedoAtlas;

layout(push_constant) uniform GbPush {
    mat4 gbViewProj;
    vec4 gbCameraPos;
};

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in float inAo;
layout(location = 4) flat in uint inMat;

layout(location = 0) out vec4 outAlbedoAo;
layout(location = 1) out vec4 outNormalRough;
layout(location = 2) out vec2 outDistMfp;

void main() {
    SharcMaterial mat = gb_materials[inMat];
    vec3 albedo = mat.albedoRough.rgb;
    float layerPlus1 = mat.mfpPad.y;
    if (layerPlus1 > 0.5) {
        albedo *= texture(gbAlbedoAtlas, vec3(inUv, layerPlus1 - 1.0)).rgb;
    }
    vec3 n = normalize(inNormal);
    // 裏面ヒットは視線側へ (hit パスと同じ規約)
    vec3 toCam = gbCameraPos.xyz - inWorldPos;
    if (dot(n, toCam) < 0.0) n = -n;

    // 自己発光: AO チャネルに 1+強度 でエンコード (a > 1 = 発光体)。
    // resolve が albedo × 強度 を放射輝度へ直接加算する
    float aoOrEmissive = (mat.mfpPad.z > 0.0) ? 1.0 + mat.mfpPad.z : inAo;
    outAlbedoAo    = vec4(albedo, aoOrEmissive);
    outNormalRough = vec4(n, mat.albedoRough.a);
    outDistMfp     = vec2(length(toCam), mat.mfpPad.x);
}
