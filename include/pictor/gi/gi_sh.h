#pragma once

/// L2 球面調和 (SH、 9 係数) ユーティリティ。
///
/// GI の間接光表現 (`spec/feature/gi-bake-realtime-design.md`) の共通基盤。
/// レイアウトは `BakedIrradiance::sh` / probe SSBO と同一の
/// 「係数 × vec4 (RGB + 予約)」 = float[9*4]。 GPU 側
/// (`shaders/gi_probe_sample.comp`) とバイト互換。
///
/// 全て純関数 (状態なし・決定的) — headless テスト対象。

#include "pictor/core/types.h"

#include <cmath>

namespace pictor {

/// SH L2 の係数数 (l=0..2, m=-l..l)。
constexpr uint32_t kSHCoeffCount = 9;

/// RGB SH 1 組の float 数 (9 係数 × vec4)。
constexpr uint32_t kSHFloatCount = kSHCoeffCount * 4;

/// 方向 `dir` (正規化済み) の SH L2 基底関数 9 個を評価する。
/// 実数 SH の標準定数 (Ramamoorthi & Hanrahan 2001)。
inline void sh_eval_basis(const float3& dir, float out[kSHCoeffCount]) {
    const float x = dir.x, y = dir.y, z = dir.z;
    out[0] = 0.282095f;                        // Y00
    out[1] = 0.488603f * y;                    // Y1-1
    out[2] = 0.488603f * z;                    // Y10
    out[3] = 0.488603f * x;                    // Y11
    out[4] = 1.092548f * x * y;                // Y2-2
    out[5] = 1.092548f * y * z;                // Y2-1
    out[6] = 0.315392f * (3.0f * z * z - 1.0f);// Y20
    out[7] = 1.092548f * x * z;                // Y21
    out[8] = 0.546274f * (x * x - y * y);      // Y22
}

/// 方向 `dir` からの放射輝度 `rgb` を SH へ射影して `sh` (float[36]) に
/// 加算する。 `weight` はサンプル重み (モンテカルロなら 4π/N など)。
inline void sh_add_radiance(float sh[kSHFloatCount], const float3& dir,
                            const float3& rgb, float weight) {
    float basis[kSHCoeffCount];
    sh_eval_basis(dir, basis);
    for (uint32_t c = 0; c < kSHCoeffCount; ++c) {
        const float b = basis[c] * weight;
        sh[c * 4 + 0] += rgb.x * b;
        sh[c * 4 + 1] += rgb.y * b;
        sh[c * 4 + 2] += rgb.z * b;
    }
}

/// SH 放射輝度から法線 `normal` (正規化済み) 方向の irradiance を評価する。
/// cosine lobe 畳み込み (A0=π, A1=2π/3, A2=π/4) を含み、 1/π で除して
/// Lambert 反射の出射輝度スケールに合わせる。
inline float3 sh_eval_irradiance(const float sh[kSHFloatCount],
                                 const float3& normal) {
    // 畳み込み済み帯係数 / π。
    constexpr float kA0 = 1.0f;          // π   / π
    constexpr float kA1 = 2.0f / 3.0f;   // 2π/3 / π
    constexpr float kA2 = 0.25f;         // π/4 / π
    const float band[kSHCoeffCount] = {kA0, kA1, kA1, kA1,
                                       kA2, kA2, kA2, kA2, kA2};
    float basis[kSHCoeffCount];
    sh_eval_basis(normal, basis);

    float3 result{};
    for (uint32_t c = 0; c < kSHCoeffCount; ++c) {
        const float b = basis[c] * band[c];
        result.x += sh[c * 4 + 0] * b;
        result.y += sh[c * 4 + 1] * b;
        result.z += sh[c * 4 + 2] * b;
    }
    result.x = std::max(0.0f, result.x);
    result.y = std::max(0.0f, result.y);
    result.z = std::max(0.0f, result.z);
    return result;
}

/// fibonacci sphere — 単位球面上に決定的で概均一な `count` 方向を生成し、
/// `index` 番目を返す。 bake / probe 構築のサンプル方向に使う
/// (乱数を使わない — テスト決定性、 RULE_CODE §16)。
inline float3 fibonacci_sphere_dir(uint32_t index, uint32_t count) {
    constexpr float kGoldenAngle = 2.39996322972865332f;
    const float n = static_cast<float>(count);
    // y を [-1, 1] に等間隔配置、 リングの角度は黄金角で回す。
    const float y = 1.0f - 2.0f * (static_cast<float>(index) + 0.5f) / n;
    const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
    const float phi = kGoldenAngle * static_cast<float>(index);
    return {r * std::cos(phi), y, r * std::sin(phi)};
}

} // namespace pictor
