// Pictor — SHaRC 拡張 (DirectX 12 版): 層2 位置つき vMF/SG ローブ (鏡面用)
//
// GLSL 正本: shaders/sharc/sharc_lobes.glsl (アルゴリズム変更禁止 — 忠実移植)。
// 依存: sharc_common.hlsli を先に include すること。

#ifndef PICTOR_SHARC_LOBES_HLSLI
#define PICTOR_SHARC_LOBES_HLSLI

struct SharcLobe {
    float3 dir;      // 単位方向 (セル中心 → 光源)
    float  kappa;    // vMF/SG 鋭さ
    float3 mu;       // SG 振幅 (RGB 放射輝度スケール)
    float  dist;     // 実効距離 (m)。 0 = 無限遠扱い
    float  weight;   // EM 混合重み π (蓄積エネルギー比)
};

static const float SHARC_KAPPA_MIN = 0.5;
static const float SHARC_KAPPA_MAX = 4096.0;

// ============================================================
// 入出力 (セル ⇔ 構造体)
// ============================================================

SharcLobe sharcLoadLobe(uint slot, uint lobeIdx) {
    uint b = sharcCellBase(slot) + SHARC_CELL_OFFSET_LOBES + lobeIdx * 3u;
    uint dirKappa = gpu_sharc_cells[b + 0u];
    SharcLobe l;
    l.dir   = sharcOct16Decode(dirKappa & 0xFFFFu);
    l.kappa = clamp(f16tof32(dirKappa >> 16), 0.0, SHARC_KAPPA_MAX);
    l.mu    = sharcRgb9e5Decode(gpu_sharc_cells[b + 1u]);
    float2 dw = sharcHalf2Unpack(gpu_sharc_cells[b + 2u]);
    l.dist   = dw.x;
    l.weight = dw.y;
    return l;
}

void sharcStoreLobe(uint slot, uint lobeIdx, SharcLobe l) {
    uint b = sharcCellBase(slot) + SHARC_CELL_OFFSET_LOBES + lobeIdx * 3u;
    // 方向 oct16 (low) + κ half (high)。 κ は half の上位 16bit へ。
    uint kappaHalf = f32tof16(clamp(l.kappa, 0.0, SHARC_KAPPA_MAX)) << 16;
    gpu_sharc_cells[b + 0u] = sharcOct16Encode(l.dir) | kappaHalf;
    gpu_sharc_cells[b + 1u] = sharcRgb9e5Encode(l.mu);
    gpu_sharc_cells[b + 2u] = sharcHalf2Pack(l.dist, l.weight);
}

// ============================================================
// SG (Spherical Gaussian) 基本演算
//   G(ω; d, κ, μ) = μ · exp(κ(ω·d − 1))
// ============================================================

float sharcSgIntegral(float kappa) {
    kappa = max(kappa, SHARC_EPSILON);
    return 2.0 * SHARC_PI * (1.0 - exp(-2.0 * kappa)) / kappa;
}

float sharcSgInnerProduct(float3 d1, float k1, float3 d2, float k2) {
    float km = length(k1 * d1 + k2 * d2);
    km = max(km, SHARC_EPSILON);
    float e = exp(km - k1 - k2) - exp(-km - k1 - k2);
    return 2.0 * SHARC_PI * e / km;
}

float sharcVmfPdf(float3 dir, float3 mean, float kappa) {
    kappa = clamp(kappa, SHARC_KAPPA_MIN, SHARC_KAPPA_MAX);
    float norm = kappa / (2.0 * SHARC_PI * (1.0 - exp(-2.0 * kappa)));
    return norm * exp(kappa * (dot(dir, mean) - 1.0));
}

// ============================================================
// GGX → SG 近似
// ============================================================

void sharcGgxAsSg(float3 n, float3 v, float roughness,
                  out float3 outDir, out float outKappa, out float outAmp) {
    float alpha = max(roughness * roughness, 0.02);
    float a2 = alpha * alpha;
    outDir   = reflect(-v, n);
    float ndv = max(dot(n, v), 0.05);
    outKappa = clamp((2.0 / a2) / (4.0 * ndv), SHARC_KAPPA_MIN, SHARC_KAPPA_MAX);
    outAmp   = 1.0 / (SHARC_PI * a2);
}

// ============================================================
// 評価 (resolve パスから使用)
// ============================================================

float3 sharcLobeReaim(SharcLobe l, float3 cellCenter, float3 shadePos,
                      out float outDistSq) {
    if (l.dist <= SHARC_EPSILON) { outDistSq = 0.0; return l.dir; }
    float3 virtualPos = cellCenter + l.dir * l.dist;
    float3 toLight = virtualPos - shadePos;
    outDistSq = dot(toLight, toLight);
    return normalize(toLight);
}

float3 sharcSpecularEvaluate(uint slot, float3 cellCenter, float3 shadePos,
                             float3 n, float3 v, float roughness) {
    float3 brdfDir; float brdfKappa; float brdfAmp;
    sharcGgxAsSg(n, v, roughness, brdfDir, brdfKappa, brdfAmp);

    float3 total = float3(0.0, 0.0, 0.0);
    for (uint i = 0u; i < uint(SHARC_LOBE_COUNT); i++) {
        SharcLobe l = sharcLoadLobe(slot, i);
        if (l.weight <= SHARC_EPSILON || sharcLuminance(l.mu) <= SHARC_EPSILON)
            continue;

        float distSq;
        float3 dir = sharcLobeReaim(l, cellCenter, shadePos, distSq);

        float falloff = 1.0;
        if (l.dist > SHARC_EPSILON) {
            falloff = (l.dist * l.dist) / max(distSq, 1e-4);
            falloff = min(falloff, 16.0);
        }

        float ip = sharcSgInnerProduct(dir, l.kappa, brdfDir, brdfKappa);
        total += l.mu * (brdfAmp * ip * falloff);
    }
    return total;
}

// ============================================================
// フィット (update パスから使用): バッチ E ステップ + EMA M ステップ
// ============================================================

void sharcLobeResponsibilities(SharcLobe lobes[SHARC_LOBE_COUNT], float3 dir,
                               out float r[SHARC_LOBE_COUNT],
                               out bool outOrphan) {
    float sum = 0.0;
    for (int k = 0; k < SHARC_LOBE_COUNT; k++) {
        float pdf = (lobes[k].weight > SHARC_EPSILON)
                  ? lobes[k].weight * sharcVmfPdf(dir, lobes[k].dir, lobes[k].kappa)
                  : 0.0;
        r[k] = pdf;
        sum += pdf;
    }
    outOrphan = (sum < 1e-5);
    float inv = (sum > SHARC_EPSILON) ? (1.0 / sum) : 0.0;
    for (int k = 0; k < SHARC_LOBE_COUNT; k++) r[k] *= inv;
}

float sharcKappaFromMeanResultant(float rBar) {
    rBar = clamp(rBar, 0.0, 0.999);
    return clamp(rBar * (3.0 - rBar * rBar) / (1.0 - rBar * rBar),
                 SHARC_KAPPA_MIN, SHARC_KAPPA_MAX);
}

SharcLobe sharcLobeBlendBatch(SharcLobe prev, float sumW, float3 sumWDir,
                              float3 sumWRgb, float sumWDist, float alpha) {
    if (sumW <= SHARC_EPSILON) {
        prev.weight *= (1.0 - alpha);
        return prev;
    }
    float rBar = length(sumWDir) / sumW;
    float3 dirNew = (length(sumWDir) > SHARC_EPSILON)
                  ? normalize(sumWDir) : prev.dir;
    float kappaNew = sharcKappaFromMeanResultant(rBar);
    float3 muNew = (sumWRgb / sumW) * (1.0 / max(sharcSgIntegral(kappaNew), SHARC_EPSILON))
                 * 2.0 * SHARC_PI;
    float distNew = sumWDist / sumW;

    SharcLobe l;
    l.dir    = normalize(lerp(prev.dir, dirNew, alpha) + float3(SHARC_EPSILON, SHARC_EPSILON, SHARC_EPSILON));
    l.kappa  = lerp(max(prev.kappa, SHARC_KAPPA_MIN), kappaNew, alpha);
    l.mu     = lerp(prev.mu, muNew, alpha);
    l.dist   = lerp(prev.dist, distNew, alpha);
    l.weight = lerp(prev.weight, sumW, alpha);
    return l;
}

void sharcLobeSpawn(inout SharcLobe lobes[SHARC_LOBE_COUNT], float3 dir,
                    float3 radiance, float hitDist, float weight) {
    int weakest = 0;
    float wMin = lobes[0].weight * sharcLuminance(lobes[0].mu);
    for (int k = 1; k < SHARC_LOBE_COUNT; k++) {
        float e = lobes[k].weight * sharcLuminance(lobes[k].mu);
        if (e < wMin) { wMin = e; weakest = k; }
    }
    if (sharcLuminance(radiance) * weight < wMin * 2.0) return;

    SharcLobe l;
    l.dir    = dir;
    l.kappa  = 32.0;
    l.mu     = radiance;
    l.dist   = hitDist;
    l.weight = weight;
    lobes[weakest] = l;
}

#endif // PICTOR_SHARC_LOBES_HLSLI
