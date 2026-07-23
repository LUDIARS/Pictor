// Pictor — SHaRC 拡張 (DirectX 12 版): 層1 モーメント M0/M1 (SSS・拡散用)
//
// GLSL 正本: shaders/sharc/sharc_moments.glsl (アルゴリズム変更禁止 — 忠実移植)。
// 依存: sharc_common.hlsli を先に include すること。

#ifndef PICTOR_SHARC_MOMENTS_HLSLI
#define PICTOR_SHARC_MOMENTS_HLSLI

// ============================================================
// 蓄積 (update パスから使用)
// ============================================================

void sharcMomentsAccumulate(inout float3 m0, inout float3 m1Vec,
                            float3 dir, float3 radiance, float weight) {
    m0    += radiance * weight;
    m1Vec += dir * (sharcLuminance(radiance) * weight);
}

void sharcMomentsBlend(uint slot, float3 m0New, float3 m1VecNew, float alpha) {
    float3 m0Old, m1DirOld, m1MagOld;
    float confOld;
    sharcLoadMoments(slot, m0Old, m1DirOld, m1MagOld, confOld);

    float lumaNew = sharcLuminance(m0New);
    float gNew = (lumaNew > SHARC_EPSILON)
               ? clamp(length(m1VecNew) / lumaNew, 0.0, 1.0) : 0.0;
    float3 dirNew = (length(m1VecNew) > SHARC_EPSILON)
                  ? normalize(m1VecNew) : m1DirOld;

    float3 m0B  = lerp(m0Old, m0New, alpha);
    float3 magB = lerp(m1MagOld, m0New * gNew, alpha);
    float3 dirB = normalize(lerp(m1DirOld, dirNew, alpha) + float3(SHARC_EPSILON, SHARC_EPSILON, SHARC_EPSILON));
    float confB = lerp(confOld, gNew, alpha);

    sharcStoreMoments(slot, m0B, dirB, magB, confB);
}

// ============================================================
// 評価 — 拡散 (resolve パスから使用)
// ============================================================

float3 sharcDiffuseIrradiance(float3 m0, float3 m1Dir, float3 m1Mag, float3 n) {
    float3 e = m1Mag * dot(m1Dir, n);
    return max(0.25 * m0 + 0.5 * e, float3(0.0, 0.0, 0.0));
}

float3 sharcDiffuseIrradianceAt(float3 worldPos, float3 n, uint level) {
    uint slot = sharcFindSlot(sharcGridCoord(worldPos, level), level);
    if (slot == SHARC_SLOT_INVALID) return float3(0.0, 0.0, 0.0);
    float3 m0, m1Dir, m1Mag; float conf;
    sharcLoadMoments(slot, m0, m1Dir, m1Mag, conf);
    return sharcDiffuseIrradiance(m0, m1Dir, m1Mag, n);
}

// ============================================================
// 評価 — SSS (resolve パスから使用)
// ============================================================

float sharcBurleyScaling(float albedo) {
    float a = clamp(albedo, 0.0, 1.0);
    float t = a - 0.8;
    return 1.85 - a + 7.0 * abs(t * t * t);
}

float sharcBurleyProfile(float r, float d) {
    r = max(r, 1e-4);
    d = max(d, 1e-4);
    return (exp(-r / d) + exp(-r / (3.0 * d))) / (8.0 * SHARC_PI * d * r);
}

float3 sharcSssGather(float3 worldPos, float3 n, float mfp, float albedoLuma) {
    uint level = sharcLevelFromMfp(mfp);
    float cell = sharcCellSize(level);
    float s = sharcBurleyScaling(albedoLuma);
    float d = max(mfp, 1e-4) / s;

    int3 center = sharcGridCoord(worldPos, level);
    int3 kTaps[7] = {
        int3(0, 0, 0),
        int3( 1, 0, 0), int3(-1, 0, 0),
        int3(0,  1, 0), int3(0, -1, 0),
        int3(0, 0,  1), int3(0, 0, -1)
    };

    float3 sum = float3(0.0, 0.0, 0.0);
    float wSum = 0.0;
    for (int i = 0; i < 7; i++) {
        int3 g = center + kTaps[i];
        uint slot = sharcFindSlot(g, level);
        if (slot == SHARC_SLOT_INVALID) continue;

        float3 m0, m1Dir, m1Mag; float conf;
        sharcLoadMoments(slot, m0, m1Dir, m1Mag, conf);

        float r = length(sharcCellCenter(g, level) - worldPos);
        if (i == 0) r = 0.25 * cell;
        float w = sharcBurleyProfile(r, d) * cell * cell * cell;

        float3 through = max(0.25 * m0 + 0.5 * m1Mag * dot(m1Dir, -n), float3(0.0, 0.0, 0.0));
        sum  += through * w;
        wSum += w;
    }
    return sum;
}

#endif // PICTOR_SHARC_MOMENTS_HLSLI
