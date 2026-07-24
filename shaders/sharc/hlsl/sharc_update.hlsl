// Pictor — SHaRC 拡張 (DirectX 12 版) Pass 3: Light (セル更新)
//
// GLSL 正本: shaders/sharc/sharc_update.comp (アルゴリズム変更禁止 — 忠実移植)。
// 1 threadgroup = 1 要求セル。
//   1. per-cell reservoir (ReSTIR DI 簡易版) からライトを importance sampling
//   2. (アンシャドウ既定)
//   3. M0/M1 蓄積 + vMF ローブ EM 更新 → EMA でセルへブレンド
//
// dispatch: ExecuteIndirect (compact が生成, x = 要求セル数)

#include "sharc_common.hlsli"
#include "sharc_moments.hlsli"
#include "sharc_lobes.hlsli"

static const uint SHARC_SAMPLES_PER_CELL = 64u;   // = numthreads.x

// ============================================================
// 乱数 (フレーム/セル/スレッドで独立)
// ============================================================

float sharcRand(inout uint state) {
    state = sharcHashUint(state);
    return float(state) * (1.0 / 4294967296.0);
}

// ============================================================
// groupshared: サンプルバッファ + 並列縮約バッファ
// ============================================================

static const uint SHARC_PART_FLOATS = uint(SHARC_LOBE_COUNT) * 8u + 7u;

groupshared float4 sh_dirLuma[SHARC_SAMPLES_PER_CELL];   // xyz=方向 w=輝度重み
groupshared float4 sh_rgbDist[SHARC_SAMPLES_PER_CELL];   // rgb=放射輝度 w=距離
groupshared float  sh_weight[SHARC_SAMPLES_PER_CELL];    // RIS 重み
groupshared uint   sh_candidate[SHARC_SAMPLES_PER_CELL]; // 候補ライト
groupshared float  sh_pdfHat[SHARC_SAMPLES_PER_CELL];    // 候補の target pdf
groupshared float  sh_part[SHARC_SAMPLES_PER_CELL][SHARC_PART_FLOATS];
groupshared float  sh_orphanScore[SHARC_SAMPLES_PER_CELL]; // 孤児サンプルの強度

// セル中心での未遮蔽寄与 (target pdf 用)
float3 sharcLightContribution(SharcLight light, float3 cellCenter,
                              out float3 outDir, out float outDist) {
    float3 toLight = light.posRadius.xyz - cellCenter;
    float d2 = max(dot(toLight, toLight), 1e-4);
    outDist = sqrt(d2);
    outDir  = toLight / outDist;
    return light.colorIntensity.rgb * light.colorIntensity.a / d2;
}

[numthreads(64, 1, 1)]
void main(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    uint reqIdx = gid.x;
    uint tid    = gtid.x;
    uint slot   = gpu_sharc_requests[reqIdx];
    // slot は threadgroup 内一様 (reqIdx = gid.x は group 内で全 thread 共通)。
    // FXC は resource load 由来の分岐を非一様と保守的に扱い、 早期 return の
    // 先に barrier があるとエラーにするため、 早期 return はせず `active`
    // フラグでガードする (barrier 自体は常に全 thread が到達する形を保つ)。
    bool active = (slot < sharcTableSize) && (gpu_sharc_keys[slot] != 0u);

    uint sampleCountPrev = 0u, lastFrame = 0u, level = 0u;
    float lumaMean = 0.0, lumaVar = 0.0;
    int3 grid = int3(0, 0, 0);
    uint posLevel = 0u;
    float3 cellCenter = float3(0.0, 0.0, 0.0);
    uint4 resPrev = uint4(0, 0, 0, 0);
    uint  resLight = 0u;
    float resM   = 0.0;
    float resPdf = 0.0;
    uint  rng = 0u;

    if (active) {
        sharcLoadMeta(slot, sampleCountPrev, lastFrame, lumaMean, lumaVar, level);
        // セル中心: march が挿入時に書いた逆引き座標から復元
        sharcUnpackGridLevel(gpu_sharc_cell_pos[slot], grid, posLevel);
        cellCenter = sharcCellCenter(grid, posLevel);

        resPrev  = gpu_sharc_reservoirs[slot];
        resLight = resPrev.x;
        resM     = asfloat(resPrev.z);
        resPdf   = asfloat(resPrev.w);
        rng = sharcHashCombine(sharcHashCombine(slot, sharcFrameIndex), tid);
    }

    // ============================================================
    // 1. 候補生成 (thread 並列)
    // ============================================================
    uint  candIdx = 0u;
    float candPdfHat = 0.0;
    float candW = 0.0;
    float3 candDir = float3(0.0, 1.0, 0.0);
    float3 candRgb = float3(0.0, 0.0, 0.0);
    float candDist = 0.0;

    if (active && sharcLightCount > 0u) {
        candIdx = min(uint(sharcRand(rng) * float(sharcLightCount)), sharcLightCount - 1u);
        float pSource = 1.0 / float(sharcLightCount);
        SharcLight light = gpu_sharc_lights[candIdx];
        float3 dir; float dist;
        float3 contrib = sharcLightContribution(light, cellCenter, dir, dist);
        candPdfHat = sharcLuminance(contrib);
        candW = candPdfHat / pSource;
        candDir = dir;
        candRgb = contrib;
        candDist = dist;
    }

    sh_dirLuma[tid]   = float4(candDir, sharcLuminance(candRgb));
    sh_rgbDist[tid]   = float4(candRgb, candDist);
    sh_weight[tid]    = candW;
    sh_candidate[tid] = candIdx;
    sh_pdfHat[tid]    = candPdfHat;
    GroupMemoryBarrierWithGroupSync();

    // ============================================================
    // 2. 各スレッドが自分のサンプルの帰属度と部分和を計算
    // ============================================================
    SharcLobe lobes[SHARC_LOBE_COUNT];
    for (int k = 0; k < SHARC_LOBE_COUNT; k++) {
        lobes[k] = sharcLoadLobe(slot, uint(k));
    }

    for (uint j = 0u; j < SHARC_PART_FLOATS; j++) sh_part[tid][j] = 0.0;
    sh_orphanScore[tid] = 0.0;

    {
        float w = sh_weight[tid];
        if (w > SHARC_EPSILON) {
            float3 dir  = sh_dirLuma[tid].xyz;
            float3 rgb  = sh_rgbDist[tid].rgb;
            float dist = sh_rgbDist[tid].w;

            float r[SHARC_LOBE_COUNT];
            bool orphan;
            sharcLobeResponsibilities(lobes, dir, r, orphan);
            if (orphan) {
                sh_orphanScore[tid] = sharcLuminance(rgb) * w;
            } else {
                for (int k = 0; k < SHARC_LOBE_COUNT; k++) {
                    float rw = r[k] * w;
                    uint b = uint(k) * 8u;
                    sh_part[tid][b + 0u] = rw;
                    sh_part[tid][b + 1u] = dir.x * rw;
                    sh_part[tid][b + 2u] = dir.y * rw;
                    sh_part[tid][b + 3u] = dir.z * rw;
                    sh_part[tid][b + 4u] = rgb.r * rw;
                    sh_part[tid][b + 5u] = rgb.g * rw;
                    sh_part[tid][b + 6u] = rgb.b * rw;
                    sh_part[tid][b + 7u] = dist * rw;
                }
            }
            // モーメント部分和 (RIS: f/p̂ を重み w で平均)
            float invPdf = w / max(sh_pdfHat[tid], SHARC_EPSILON);
            uint mb = uint(SHARC_LOBE_COUNT) * 8u;
            sh_part[tid][mb + 0u] = rgb.r * invPdf;
            sh_part[tid][mb + 1u] = rgb.g * invPdf;
            sh_part[tid][mb + 2u] = rgb.b * invPdf;
            float luma = sharcLuminance(rgb) * invPdf;
            sh_part[tid][mb + 3u] = dir.x * luma;
            sh_part[tid][mb + 4u] = dir.y * luma;
            sh_part[tid][mb + 5u] = dir.z * luma;
            sh_part[tid][mb + 6u] = invPdf;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // ============================================================
    // 3. ツリー縮約 (log2(64) = 6 ステップ)
    // ============================================================
    for (uint stride = SHARC_SAMPLES_PER_CELL / 2u; stride > 0u; stride >>= 1u) {
        if (tid < stride) {
            for (uint j = 0u; j < SHARC_PART_FLOATS; j++) {
                sh_part[tid][j] += sh_part[tid + stride][j];
            }
            // 孤児は最強 1 サンプルだけ残す (max 縮約)
            if (sh_orphanScore[tid + stride] > sh_orphanScore[tid]) {
                sh_orphanScore[tid] = sh_orphanScore[tid + stride];
                sh_dirLuma[tid]     = sh_dirLuma[tid + stride];
                sh_rgbDist[tid]     = sh_rgbDist[tid + stride];
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // ============================================================
    // 4. 仕上げ (thread 0 のみ。 barrier は既に全て終えているので
    //    return ではなく if で括っても分岐上の問題はない)
    // ============================================================
    if (tid == 0u && active) {

    // --- WRS: 今フレーム候補を reservoir へ統合 ---
    float wTotal = 0.0;
    for (uint i = 0u; i < SHARC_SAMPLES_PER_CELL; i++) wTotal += sh_weight[i];

    float pick = sharcRand(rng) * max(wTotal, SHARC_EPSILON);
    uint  chosen = 0u;
    float acc = 0.0;
    for (uint i = 0u; i < SHARC_SAMPLES_PER_CELL; i++) {
        acc += sh_weight[i];
        if (acc >= pick) { chosen = i; break; }
    }
    const float kMaxM = 512.0;
    float mNew = float(SHARC_SAMPLES_PER_CELL);
    if (resM > 0.0 && resPdf > 0.0) {
        float wPrev = resPdf * resM;
        wTotal += wPrev;
        if (sharcRand(rng) * wTotal < wPrev) {
            chosen = 0xFFFFFFFFu;   // 既存採択
        }
    }
    uint  outLight; float outPdf;
    if (chosen == 0xFFFFFFFFu) {
        outLight = resLight;
        outPdf   = resPdf;
    } else {
        outLight = sh_candidate[chosen];
        outPdf   = sh_pdfHat[chosen];
    }
    gpu_sharc_reservoirs[slot] =
        uint4(outLight, asuint(wTotal), asuint(min(resM + mNew, kMaxM)), asuint(outPdf));

    // --- モーメント EMA (縮約済み部分和から) ---
    uint mb = uint(SHARC_LOBE_COUNT) * 8u;
    float wNorm = sh_part[0][mb + 6u];
    float3 m0 = float3(0.0, 0.0, 0.0);
    if (wNorm > SHARC_EPSILON) {
        m0 = float3(sh_part[0][mb + 0u], sh_part[0][mb + 1u], sh_part[0][mb + 2u]) / wNorm;
        float3 m1 = float3(sh_part[0][mb + 3u], sh_part[0][mb + 4u], sh_part[0][mb + 5u]) / wNorm;
        sharcMomentsBlend(slot, m0, m1, sharcEmaAlpha);
    }

    // --- ローブ EMA (縮約済みバッチ統計から) + 孤児 1 本スポーン ---
    if (sh_orphanScore[0] > SHARC_EPSILON) {
        sharcLobeSpawn(lobes, sh_dirLuma[0].xyz, sh_rgbDist[0].rgb, sh_rgbDist[0].w, sh_orphanScore[0]);
    }
    for (int k = 0; k < SHARC_LOBE_COUNT; k++) {
        uint b = uint(k) * 8u;
        lobes[k] = sharcLobeBlendBatch(
            lobes[k], sh_part[0][b + 0u],
            float3(sh_part[0][b + 1u], sh_part[0][b + 2u], sh_part[0][b + 3u]),
            float3(sh_part[0][b + 4u], sh_part[0][b + 5u], sh_part[0][b + 6u]),
            sh_part[0][b + 7u], sharcEmaAlpha);
        sharcStoreLobe(slot, uint(k), lobes[k]);
    }

    // --- メタ更新 ---
    float lumaBatch = sharcLuminance(m0);
    float meanNew = lerp(lumaMean, lumaBatch, sharcEmaAlpha);
    float varNew  = lerp(lumaVar, (lumaBatch - meanNew) * (lumaBatch - meanNew), sharcEmaAlpha);
    sharcStoreMeta(slot, sampleCountPrev + SHARC_SAMPLES_PER_CELL,
                   sharcFrameIndex & 0xFFFFu, meanNew, varNew, level);
    } // if (tid == 0u && active)
}
