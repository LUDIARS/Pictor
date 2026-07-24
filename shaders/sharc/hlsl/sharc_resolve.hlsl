// Pictor — SHaRC 拡張 (DirectX 12 版) Pass 4: Resolve
//
// GLSL 正本: shaders/sharc/sharc_resolve.comp (アルゴリズム変更禁止 — 忠実移植)。
// キャッシュ参照のみでシェーディングを閉じる (decoupled shading)。
//
// dispatch: (rayCount / 64, 1, 1)

#include "sharc_common.hlsli"
#include "sharc_moments.hlsli"
#include "sharc_lobes.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint rayIdx = dtid.x;
    if (rayIdx >= sharcRayCount) return;

    SharcShadeRequest req = gpu_sharc_shade[rayIdx];
    float3 pos       = req.posRough.xyz;
    float roughness = req.posRough.w;
    float3 n         = normalize(req.normalMfp.xyz);
    float mfp       = req.normalMfp.w;
    float3 albedo    = req.albedoView.rgb;
    float3 v         = normalize(req.viewDir.xyz);

    uint levelBase = sharcLevelFromDistance(distance(pos, sharcCameraPos.xyz));

    float3 radiance = float3(0.0, 0.0, 0.0);
    float hit = 0.0;

    // --- 拡散 + 鏡面: 8 近傍セルのトライリニア補間評価 ---
    float ndv = max(dot(n, v), SHARC_EPSILON);
    float fresnel = 0.04 + 0.96 * pow(1.0 - ndv, 5.0);

    for (uint attempt = 0u; attempt < 3u && hit == 0.0; attempt++) {
        // attempt 0: levelBase / 1: levelBase-1 / 2: levelBase+1
        uint level = levelBase;
        if (attempt == 1u) {
            if (levelBase == 0u) continue;
            level = levelBase - 1u;
        } else if (attempt == 2u) {
            if (levelBase + 1u >= sharcLevelCount) continue;
            level = levelBase + 1u;
        }

        float cell = sharcCellSize(level);
        float3 gpos = pos / cell - 0.5;
        int3 base = int3(floor(gpos));
        float3 f = gpos - float3(base);

        float3 acc = float3(0.0, 0.0, 0.0);
        float wSum = 0.0;
        for (int c = 0; c < 8; c++) {
            int3 offs = int3(c & 1, (c >> 1) & 1, (c >> 2) & 1);
            int3 grid = base + offs;
            uint slot = sharcFindSlot(grid, level);
            if (slot == SHARC_SLOT_INVALID) continue;

            float3 w3 = lerp(1.0 - f, f, float3(offs));
            float w = w3.x * w3.y * w3.z;
            if (w <= 1e-4) continue;

            // 拡散 (層1 モーメント)
            float3 m0, m1Dir, m1Mag; float conf;
            sharcLoadMoments(slot, m0, m1Dir, m1Mag, conf);
            float3 sample_ = albedo * SHARC_INV_PI
                            * sharcDiffuseIrradiance(m0, m1Dir, m1Mag, n);

            // 鏡面 (層2 ローブ、 セルごとの中心で再照準)
            float3 cellCenter = sharcCellCenter(grid, level);
            sample_ += fresnel * sharcSpecularEvaluate(slot, cellCenter, pos, n, v, roughness);

            acc  += sample_ * w;
            wSum += w;
        }
        if (wSum > 1e-4) {
            radiance += acc / wSum;
            hit = 1.0;
        }
    }

    // --- SSS (層1, MFP レベルのギャザー) ---
    if (mfp > 0.0) {
        float3 sss = sharcSssGather(pos, n, mfp, sharcLuminance(albedo));
        radiance += albedo * sss;
    }

    gpu_sharc_output[rayIdx] = float4(radiance, hit);
}
