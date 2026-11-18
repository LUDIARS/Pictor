#pragma once
// =============================================================================
// CPU 版 Gerstner 波 (ocean.frag と同じ式)。
// 「波の高さを CPU からアクセスできる」= 船を水面に浮かべる (浮力表現) のに使う。
// GPU と同じ式・同じパラメータで計算しないと、船が水面から浮いたり沈んだりする。
// ※ 浅瀬の減衰は海底関数 (ノイズ入り) まで移植していないので、船は深い場所に置く。
// =============================================================================

#include "demo_math.h"

namespace ubg {

struct OceanParams {
    float amplitude   = 1.0f;   // p0.x
    float steepness   = 0.6f;   // p0.y
    float wave_scale  = 1.0f;   // p0.z
    float wind_deg    = 30.0f;  // p0.w
    float fade_start  = 150.0f; // p2.y
    float fade_end    = 400.0f; // p2.z
};

namespace gerstner_detail {
constexpr int   kNW = 5;
constexpr float kOffs[kNW] = {0.0f, 0.55f, -0.45f, 1.1f, -1.25f};
constexpr float kLens[kNW] = {30.0f, 17.0f, 11.0f, 7.0f, 4.3f};
}

/// 格子点 (x0, z0) を動かした位置
inline Vec3 gerstner_cpu(const OceanParams& op, float x0, float z0, float t, float cam_x, float cam_z) {
    using namespace gerstner_detail;
    Vec3 p{x0, 0.0f, z0};
    float d = std::sqrt((x0 - cam_x) * (x0 - cam_x) + (z0 - cam_z) * (z0 - cam_z));
    float sc = 1.0f - smoothstepf(op.fade_start, op.fade_end, d);
    for (int i = 0; i < kNW; ++i) {
        float ang = op.wind_deg * kPi / 180.0f + kOffs[i];
        float Dx = std::cos(ang), Dz = std::sin(ang);
        float L = kLens[i] * op.wave_scale;
        float A = L * 0.010f * op.amplitude * sc;
        float k = 2.0f * kPi / L;
        float w = std::sqrt(9.81f * k);
        float th = k * (Dx * x0 + Dz * z0) - w * t;
        float denom = std::fmax(k * A * kNW, 1e-4f);
        float Q = A > 1e-5f ? std::fmin(op.steepness / denom, 1.0f / denom) : 0.0f;
        p.x += Q * A * Dx * std::cos(th);
        p.z += Q * A * Dz * std::cos(th);
        p.y += A * std::sin(th);
    }
    return p;
}

/// ワールド (x, z) の水面の高さ (横ずれを打ち消す逆算つき)
inline float water_height_cpu(const OceanParams& op, float x, float z, float t, float cam_x, float cam_z) {
    float x0 = x, z0 = z;
    for (int i = 0; i < 3; ++i) {
        Vec3 p = gerstner_cpu(op, x0, z0, t, cam_x, cam_z);
        x0 -= (p.x - x);
        z0 -= (p.z - z);
    }
    return gerstner_cpu(op, x0, z0, t, cam_x, cam_z).y;
}

} // namespace ubg
