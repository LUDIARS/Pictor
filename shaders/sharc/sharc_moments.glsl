// Pictor — SHaRC 拡張: 層1 モーメント M0/M1 (SSS・拡散用)
//
// M0 = fluence φ = ∫L dω (RGB), M1 = ベクトル放射照度 E = ∫L ω dω。
// LPV (SH L1) と数学的に同一 (spec §2)。 SSS には高周波角度情報が不要
// なため、この 2 モーメントで拡散近似 (Burley normalized diffusion) が閉じる。
//
// 依存: sharc_common.glsl を先に include すること。

#ifndef PICTOR_SHARC_MOMENTS_GLSL
#define PICTOR_SHARC_MOMENTS_GLSL

// ============================================================
// 蓄積 (update パスから使用)
// ============================================================

// 1 サンプル (方向 dir から放射輝度 L, 重み w) のモーメント寄与。
// m1Vec は輝度重みの正味フラックス方向 (Σ dir·luma·w)。
// 呼び出し側は shared メモリで総和し sharcMomentsBlend() で EMA する。
void sharcMomentsAccumulate(inout vec3 m0, inout vec3 m1Vec,
                            vec3 dir, vec3 radiance, float weight) {
    m0    += radiance * weight;
    m1Vec += dir * (sharcLuminance(radiance) * weight);
}

// フレーム蓄積分 (m0New, m1VecNew) を既存セルへ EMA ブレンドして格納形式に変換。
//   m1Vec (輝度重みの正味フラックス方向) → 方向 oct16 + 大きさ RGB9E5。
//   大きさは |E| を RGB で保持: 方向への射影比 (異方性 g = |m1Vec|/luma(m0))
//   を各チャネルへ一様適用する近似。
void sharcMomentsBlend(uint slot, vec3 m0New, vec3 m1VecNew, float alpha) {
    vec3 m0Old, m1DirOld, m1MagOld;
    float confOld;
    sharcLoadMoments(slot, m0Old, m1DirOld, m1MagOld, confOld);

    float lumaNew = sharcLuminance(m0New);
    float gNew = (lumaNew > SHARC_EPSILON)
               ? clamp(length(m1VecNew) / lumaNew, 0.0, 1.0) : 0.0;
    vec3 dirNew = (length(m1VecNew) > SHARC_EPSILON)
                ? normalize(m1VecNew) : m1DirOld;

    vec3 m0B = mix(m0Old, m0New, alpha);
    vec3 magB = mix(m1MagOld, m0New * gNew, alpha);
    // 方向はベクトル合成で補間 (符号反転時の破綻を避ける)
    vec3 dirB = normalize(mix(m1DirOld, dirNew, alpha) + vec3(SHARC_EPSILON));
    float confB = mix(confOld, gNew, alpha);

    sharcStoreMoments(slot, m0B, dirB, magB, confB);
}

// ============================================================
// 評価 — 拡散 (resolve パスから使用)
// ============================================================

// SH L1 相当の放射輝度再構成 L(ω) ≈ (φ + 3 E·ω) / 4π から、
// 法線 n の半球放射照度を閉形式で得る:
//   I(n) = φ/4 + (E·n)/2
vec3 sharcDiffuseIrradiance(vec3 m0, vec3 m1Dir, vec3 m1Mag, vec3 n) {
    vec3 e = m1Mag * dot(m1Dir, n);
    return max(0.25 * m0 + 0.5 * e, vec3(0.0));
}

// スロット版 (セル読み込み込み)。 セル未確保は vec3(0)。
vec3 sharcDiffuseIrradianceAt(vec3 worldPos, vec3 n, uint level) {
    uint slot = sharcFindSlot(sharcGridCoord(worldPos, level), level);
    if (slot == SHARC_SLOT_INVALID) return vec3(0.0);
    vec3 m0, m1Dir, m1Mag; float conf;
    sharcLoadMoments(slot, m0, m1Dir, m1Mag, conf);
    return sharcDiffuseIrradiance(m0, m1Dir, m1Mag, n);
}

// ============================================================
// 評価 — SSS (resolve パスから使用)
// ============================================================

// Christensen-Burley のスケーリング係数 s(A) (searchlight 構成)。
float sharcBurleyScaling(float albedo) {
    float a = clamp(albedo, 0.0, 1.0);
    float t = a - 0.8;
    return 1.85 - a + 7.0 * abs(t * t * t);
}

// Burley normalized diffusion R(r)。 d = 拡散長。
// R(r) = (e^{-r/d} + e^{-r/(3d)}) / (8π d r)
float sharcBurleyProfile(float r, float d) {
    r = max(r, 1e-4);
    d = max(d, 1e-4);
    return (exp(-r / d) + exp(-r / (3.0 * d))) / (8.0 * SHARC_PI * d * r);
}

// 表面下輸送の空間積分 (spec §2):
//   レベルはカメラ距離ではなく MFP で選択。 拡散半径 ≒ セルサイズの階層を
//   引き、 中心 + 6 近傍セルの M0/M1 読みで積分を閉じる。
//   ワールド空間キャッシュなので画面外由来の透過光も自然に入る。
// n は出射面の法線 (外向き)。 透過は正味フラックス E が -n 側から
// 流入している成分を加点する。
vec3 sharcSssGather(vec3 worldPos, vec3 n, float mfp, float albedoLuma) {
    uint level = sharcLevelFromMfp(mfp);
    float cell = sharcCellSize(level);
    float s = sharcBurleyScaling(albedoLuma);
    float d = max(mfp, 1e-4) / s;

    ivec3 center = sharcGridCoord(worldPos, level);
    const ivec3 kTaps[7] = ivec3[7](
        ivec3(0, 0, 0),
        ivec3( 1, 0, 0), ivec3(-1, 0, 0),
        ivec3(0,  1, 0), ivec3(0, -1, 0),
        ivec3(0, 0,  1), ivec3(0, 0, -1));

    vec3 sum = vec3(0.0);
    float wSum = 0.0;
    for (int i = 0; i < 7; i++) {
        ivec3 g = center + kTaps[i];
        uint slot = sharcFindSlot(g, level);
        if (slot == SHARC_SLOT_INVALID) continue;

        vec3 m0, m1Dir, m1Mag; float conf;
        sharcLoadMoments(slot, m0, m1Dir, m1Mag, conf);

        float r = length(sharcCellCenter(g, level) - worldPos);
        // セル体積内の平均距離近似: 中心タップは r=0 を避けセル半径の半分
        if (i == 0) r = 0.25 * cell;
        float w = sharcBurleyProfile(r, d) * cell * cell * cell;

        // fluence 主体 + 逆向きフラックス (透過) を方向ボーナスとして加点
        vec3 through = max(0.25 * m0 + 0.5 * m1Mag * dot(m1Dir, -n), vec3(0.0));
        sum  += through * w;
        wSum += w;
    }
    // プロファイルの正規化残差はアルベドテクスチャ側で吸収する前提の
    // 素の重み付き和 (等メモリ比較で正規化を固定するため)。
    return sum;
}

#endif // PICTOR_SHARC_MOMENTS_GLSL
