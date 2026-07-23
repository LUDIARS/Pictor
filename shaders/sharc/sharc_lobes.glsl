// Pictor — SHaRC 拡張: 層2 位置つき vMF/SG ローブ (鏡面用)
//
// 各ローブ = { 方向 (oct16), 鋭さ κ (half), 強度 μ RGB (RGB9E5),
//              実効距離 (half), EM 重み π (half) } = 12B。
// - フィット: Ruppert+ 2020 (parallax-aware vMF mixture) の incremental EM を
//   SHaRC の蓄積 + EMA 更新ループに重ねた簡約版 (バッチ E ステップ + EMA M)。
// - 評価: GGX を SG 近似し、 ローブとの積分は SG 内積の閉形式。
//   prefilter ミップチェーン不要、 roughness 対応は κ 合成のみ (spec §2)。
// - 実効距離: セル中心 + dir·dist を仮想光源位置とし、 シェーディング点から
//   再照準することでパララックス誤差・近接光源のハイライト肥大を防ぐ。
//
// 依存: sharc_common.glsl を先に include すること。

#ifndef PICTOR_SHARC_LOBES_GLSL
#define PICTOR_SHARC_LOBES_GLSL

struct SharcLobe {
    vec3  dir;      // 単位方向 (セル中心 → 光源)
    float kappa;    // vMF/SG 鋭さ
    vec3  mu;       // SG 振幅 (RGB 放射輝度スケール)
    float dist;     // 実効距離 (m)。 0 = 無限遠扱い
    float weight;   // EM 混合重み π (蓄積エネルギー比)
};

const float SHARC_KAPPA_MIN = 0.5;
const float SHARC_KAPPA_MAX = 4096.0;

// ============================================================
// 入出力 (セル ⇔ 構造体)
// ============================================================

SharcLobe sharcLoadLobe(uint slot, uint lobeIdx) {
    uint b = sharcCellBase(slot) + SHARC_CELL_OFFSET_LOBES + lobeIdx * 3u;
    uint dirKappa = gpu_sharc_cells[b + 0u];
    SharcLobe l;
    l.dir   = sharcOct16Decode(dirKappa & 0xFFFFu);
    l.kappa = clamp(unpackHalf2x16(dirKappa).y, 0.0, SHARC_KAPPA_MAX);
    l.mu    = sharcRgb9e5Decode(gpu_sharc_cells[b + 1u]);
    vec2 dw = sharcHalf2Unpack(gpu_sharc_cells[b + 2u]);
    l.dist   = dw.x;
    l.weight = dw.y;
    return l;
}

void sharcStoreLobe(uint slot, uint lobeIdx, SharcLobe l) {
    uint b = sharcCellBase(slot) + SHARC_CELL_OFFSET_LOBES + lobeIdx * 3u;
    // 方向 oct16 (low) + κ half (high)。 κ は half の上位 16bit へ。
    uint kappaHalf = packHalf2x16(vec2(0.0, clamp(l.kappa, 0.0,
                                                 SHARC_KAPPA_MAX))) & 0xFFFF0000u;
    gpu_sharc_cells[b + 0u] = sharcOct16Encode(l.dir) | kappaHalf;
    gpu_sharc_cells[b + 1u] = sharcRgb9e5Encode(l.mu);
    gpu_sharc_cells[b + 2u] = sharcHalf2Pack(l.dist, l.weight);
}

// ============================================================
// SG (Spherical Gaussian) 基本演算
//   G(ω; d, κ, μ) = μ · exp(κ(ω·d − 1))
// ============================================================

// SG の全球積分 ∫G dω = 2π μ (1 − e^{−2κ}) / κ
float sharcSgIntegral(float kappa) {
    kappa = max(kappa, SHARC_EPSILON);
    return 2.0 * SHARC_PI * (1.0 - exp(-2.0 * kappa)) / kappa;
}

// 2 つの SG の内積 ∫G1·G2 dω (閉形式)。 μ は呼び出し側で乗じる。
float sharcSgInnerProduct(vec3 d1, float k1, vec3 d2, float k2) {
    float km = length(k1 * d1 + k2 * d2);
    km = max(km, SHARC_EPSILON);
    // 2π (e^{km−k1−k2} − e^{−km−k1−k2}) / km
    float e = exp(km - k1 - k2) - exp(-km - k1 - k2);
    return 2.0 * SHARC_PI * e / km;
}

// vMF の正規化評価 (EM の尤度用): f(ω) = κ/(4π sinh κ) e^{κ ω·d}。
// κ が大きいと sinh が溢れるため log 空間で安定化した比のみ返す。
float sharcVmfPdf(vec3 dir, vec3 mean, float kappa) {
    kappa = clamp(kappa, SHARC_KAPPA_MIN, SHARC_KAPPA_MAX);
    // κ/(2π(1−e^{−2κ})) · e^{κ(ω·d−1)}  (sinh を e^κ で括った安定形)
    float norm = kappa / (2.0 * SHARC_PI * (1.0 - exp(-2.0 * kappa)));
    return norm * exp(kappa * (dot(dir, mean) - 1.0));
}

// ============================================================
// GGX → SG 近似
// ============================================================

// GGX NDF (ハーフベクトル域) の SG 近似: κ_h = 2/α², 振幅 = 1/(πα²)。
// 反射方向域への warp で κ_r = κ_h / (4|N·V|) (Wang+ 2009 ASG 系の慣例)。
void sharcGgxAsSg(vec3 n, vec3 v, float roughness,
                  out vec3 outDir, out float outKappa, out float outAmp) {
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

// パララックス再照準: ローブの仮想光源位置 = cellCenter + dir·dist を
// シェーディング点から見た方向に置き直す。 dist==0 は無限遠 (方向のまま)。
vec3 sharcLobeReaim(SharcLobe l, vec3 cellCenter, vec3 shadePos,
                    out float outDistSq) {
    if (l.dist <= SHARC_EPSILON) { outDistSq = 0.0; return l.dir; }
    vec3 virtualPos = cellCenter + l.dir * l.dist;
    vec3 toLight = virtualPos - shadePos;
    outDistSq = dot(toLight, toLight);
    return normalize(toLight);
}

// セルのローブ集合で鏡面応答を評価する。
//   BRDF 側 GGX ローブ (SG 近似) と各キャッシュローブの SG 内積の総和。
//   近接光源のハイライト肥大は再照準 + 距離減衰 (1/d²) で抑える。
vec3 sharcSpecularEvaluate(uint slot, vec3 cellCenter, vec3 shadePos,
                           vec3 n, vec3 v, float roughness) {
    vec3 brdfDir; float brdfKappa; float brdfAmp;
    sharcGgxAsSg(n, v, roughness, brdfDir, brdfKappa, brdfAmp);

    vec3 total = vec3(0.0);
    for (uint i = 0u; i < uint(SHARC_LOBE_COUNT); i++) {
        SharcLobe l = sharcLoadLobe(slot, i);
        if (l.weight <= SHARC_EPSILON || sharcLuminance(l.mu) <= SHARC_EPSILON)
            continue;

        float distSq;
        vec3 dir = sharcLobeReaim(l, cellCenter, shadePos, distSq);

        // 再照準後の距離減衰。 蓄積はセル中心基準なので比率で補正する。
        float falloff = 1.0;
        if (l.dist > SHARC_EPSILON) {
            falloff = (l.dist * l.dist) / max(distSq, 1e-4);
            falloff = min(falloff, 16.0);   // 極近接の発散防止
        }

        float ip = sharcSgInnerProduct(dir, l.kappa, brdfDir, brdfKappa);
        total += l.mu * (brdfAmp * ip * falloff);
    }
    // コサイン / フレネル項はマテリアル側 (呼び出し元) が乗じる。
    return total;
}

// ============================================================
// フィット (update パスから使用): バッチ E ステップ + EMA M ステップ
// ============================================================

// 1 サンプルの帰属度 (responsibility) を求める。 戻り値 = 正規化済み r[k]。
// 全ローブ尤度が閾値未満なら outOrphan=true (新規ローブ候補)。
void sharcLobeResponsibilities(SharcLobe lobes[SHARC_LOBE_COUNT], vec3 dir,
                               out float r[SHARC_LOBE_COUNT],
                               out bool outOrphan) {
    float sum = 0.0;
    for (int k = 0; k < SHARC_LOBE_COUNT; k++) {
        float pdf = (lobes[k].weight > SHARC_EPSILON)
                  ? lobes[k].weight * sharcVmfPdf(dir, lobes[k].dir,
                                                  lobes[k].kappa)
                  : 0.0;
        r[k] = pdf;
        sum += pdf;
    }
    outOrphan = (sum < 1e-5);
    float inv = (sum > SHARC_EPSILON) ? (1.0 / sum) : 0.0;
    for (int k = 0; k < SHARC_LOBE_COUNT; k++) r[k] *= inv;
}

// 平均合成ベクトル長 r̄ から vMF の κ を推定 (Banerjee+ 2005 近似)。
float sharcKappaFromMeanResultant(float rBar) {
    rBar = clamp(rBar, 0.0, 0.999);
    return clamp(rBar * (3.0 - rBar * rBar) / (1.0 - rBar * rBar),
                 SHARC_KAPPA_MIN, SHARC_KAPPA_MAX);
}

// バッチ統計 (E ステップ済み) から 1 ローブを EMA 更新する。
//   sumW      = Σ r·w            (帰属重み)
//   sumWDir   = Σ r·w·dir        (方向合成)
//   sumWRgb   = Σ r·w·L          (エネルギー)
//   sumWDist  = Σ r·w·hitDist    (実効距離)
//   alpha     = EMA 係数 (sharcEmaAlpha)
SharcLobe sharcLobeBlendBatch(SharcLobe prev, float sumW, vec3 sumWDir,
                              vec3 sumWRgb, float sumWDist, float alpha) {
    if (sumW <= SHARC_EPSILON) {
        // 今フレーム帰属なし → 重みだけ減衰 (エビクションは meta 側判断)
        prev.weight *= (1.0 - alpha);
        return prev;
    }
    float rBar = length(sumWDir) / sumW;
    vec3 dirNew = (length(sumWDir) > SHARC_EPSILON)
                ? normalize(sumWDir) : prev.dir;
    float kappaNew = sharcKappaFromMeanResultant(rBar);
    // 振幅: バッチ平均放射輝度を SG 積分で割って μ に正規化
    vec3 muNew = (sumWRgb / sumW) * (1.0 / max(sharcSgIntegral(kappaNew),
                                               SHARC_EPSILON)) * 2.0 * SHARC_PI;
    float distNew = sumWDist / sumW;

    SharcLobe l;
    l.dir    = normalize(mix(prev.dir, dirNew, alpha) + vec3(SHARC_EPSILON));
    l.kappa  = mix(max(prev.kappa, SHARC_KAPPA_MIN), kappaNew, alpha);
    l.mu     = mix(prev.mu, muNew, alpha);
    l.dist   = mix(prev.dist, distNew, alpha);
    l.weight = mix(prev.weight, sumW, alpha);
    return l;
}

// 孤児サンプル (どのローブにも帰属しない高エネルギー) で最弱ローブを置換。
void sharcLobeSpawn(inout SharcLobe lobes[SHARC_LOBE_COUNT], vec3 dir,
                    vec3 radiance, float hitDist, float weight) {
    int weakest = 0;
    float wMin = lobes[0].weight * sharcLuminance(lobes[0].mu);
    for (int k = 1; k < SHARC_LOBE_COUNT; k++) {
        float e = lobes[k].weight * sharcLuminance(lobes[k].mu);
        if (e < wMin) { wMin = e; weakest = k; }
    }
    // 置換閾値: 新規エネルギーが既存最弱の 2 倍以上 (チラつき防止)
    if (sharcLuminance(radiance) * weight < wMin * 2.0) return;

    SharcLobe l;
    l.dir    = dir;
    l.kappa  = 32.0;            // 初期は中程度の鋭さから EM で収束させる
    l.mu     = radiance;
    l.dist   = hitDist;
    l.weight = weight;
    lobes[weakest] = l;
}

#endif // PICTOR_SHARC_LOBES_GLSL
