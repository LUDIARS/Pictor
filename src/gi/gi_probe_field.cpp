#include "pictor/gi/gi_probe_field.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pictor {

namespace {

float3 normalize_or_zero(const float3& v) {
    const float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
    if (len2 < 1e-12f) return {0.0f, 0.0f, 0.0f};
    const float inv = 1.0f / std::sqrt(len2);
    return v * inv;
}

float length3(const float3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

float dot3(const float3& a, const float3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

} // namespace

// ============================================================
// sample_probe_grid — trilinear 補間の共通実装
// ============================================================

void sample_probe_grid(const GIProbeConfig& config, const float* sh_data,
                       const float3& pos, float out_sh[kSHFloatCount]) {
    std::memset(out_sh, 0, sizeof(float) * kSHFloatCount);
    if (!sh_data) return;

    const uint32_t gx = std::max(config.grid_x, 1u);
    const uint32_t gy = std::max(config.grid_y, 1u);
    const uint32_t gz = std::max(config.grid_z, 1u);

    // grid ローカル連続座標 (probe 間隔単位)。 端はクランプ。
    auto local = [](float p, float origin, float spacing) {
        return spacing > 1e-6f ? (p - origin) / spacing : 0.0f;
    };
    float fx = local(pos.x, config.grid_origin.x, config.grid_spacing.x);
    float fy = local(pos.y, config.grid_origin.y, config.grid_spacing.y);
    float fz = local(pos.z, config.grid_origin.z, config.grid_spacing.z);
    fx = std::clamp(fx, 0.0f, static_cast<float>(gx - 1));
    fy = std::clamp(fy, 0.0f, static_cast<float>(gy - 1));
    fz = std::clamp(fz, 0.0f, static_cast<float>(gz - 1));

    const uint32_t x0 = static_cast<uint32_t>(fx);
    const uint32_t y0 = static_cast<uint32_t>(fy);
    const uint32_t z0 = static_cast<uint32_t>(fz);
    const uint32_t x1 = std::min(x0 + 1, gx - 1);
    const uint32_t y1 = std::min(y0 + 1, gy - 1);
    const uint32_t z1 = std::min(z0 + 1, gz - 1);
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    const float tz = fz - static_cast<float>(z0);

    const uint32_t xs[2] = {x0, x1};
    const uint32_t ys[2] = {y0, y1};
    const uint32_t zs[2] = {z0, z1};
    const float wx[2] = {1.0f - tx, tx};
    const float wy[2] = {1.0f - ty, ty};
    const float wz[2] = {1.0f - tz, tz};

    for (int iz = 0; iz < 2; ++iz) {
        for (int iy = 0; iy < 2; ++iy) {
            for (int ix = 0; ix < 2; ++ix) {
                const float w = wx[ix] * wy[iy] * wz[iz];
                if (w <= 0.0f) continue;
                const uint32_t probe =
                    xs[ix] + gx * (ys[iy] + gy * zs[iz]);
                const float* src = sh_data + probe * kSHFloatCount;
                for (uint32_t c = 0; c < kSHFloatCount; ++c) {
                    out_sh[c] += src[c] * w;
                }
            }
        }
    }
}

// ============================================================
// GIProbeField
// ============================================================

float3 GIProbeField::probe_position(uint32_t x, uint32_t y, uint32_t z) const {
    return {config_.grid_origin.x + config_.grid_spacing.x * static_cast<float>(x),
            config_.grid_origin.y + config_.grid_spacing.y * static_cast<float>(y),
            config_.grid_origin.z + config_.grid_spacing.z * static_cast<float>(z)};
}

void GIProbeField::build(const GIProbeConfig& config, const GISceneProxy& proxy,
                         const DirectionalLight& sun,
                         const std::vector<PointLight>& points,
                         const BuildParams& params) {
    config_ = config;
    params_ = params;
    proxy_  = &proxy;

    const uint32_t gx = std::max(config.grid_x, 1u);
    const uint32_t gy = std::max(config.grid_y, 1u);
    const uint32_t gz = std::max(config.grid_z, 1u);
    probe_count_ = gx * gy * gz;

    const uint32_t rays = std::max(params_.rays_per_probe, 8u);

    // 遮蔽キャッシュを構築 — probe × 方向のヒット距離 (ミスは -1)。
    // レイ長は grid 対角 (probe から見える範囲として十分)。
    const float3 extent = {config.grid_spacing.x * static_cast<float>(gx),
                           config.grid_spacing.y * static_cast<float>(gy),
                           config.grid_spacing.z * static_cast<float>(gz)};
    const float ray_len = std::max(1.0f, length3(extent));

    ray_hit_dist_.assign(static_cast<size_t>(probe_count_) * rays, -1.0f);
    for (uint32_t z = 0; z < gz; ++z) {
        for (uint32_t y = 0; y < gy; ++y) {
            for (uint32_t x = 0; x < gx; ++x) {
                const uint32_t p = x + gx * (y + gy * z);
                const float3 pos = probe_position(x, y, z);
                float* dist = ray_hit_dist_.data()
                            + static_cast<size_t>(p) * rays;
                for (uint32_t r = 0; r < rays; ++r) {
                    const float3 dir = fibonacci_sphere_dir(r, rays);
                    const auto hit = proxy.closest_hit(pos, dir, ray_len);
                    dist[r] = hit.hit ? hit.distance : -1.0f;
                }
            }
        }
    }

    sh_.assign(static_cast<size_t>(probe_count_) * kSHFloatCount, 0.0f);
    built_ = true;

    relight(sun, points);
}

void GIProbeField::relight(const DirectionalLight& sun,
                           const std::vector<PointLight>& points) {
    if (!built_ || proxy_ == nullptr) return;

    for (uint32_t p = 0; p < probe_count_; ++p) {
        relight_probe(p, sun, points);
    }
}

void GIProbeField::relight_probe(uint32_t index, const DirectionalLight& sun,
                                 const std::vector<PointLight>& points) {
    const uint32_t gx = std::max(config_.grid_x, 1u);
    const uint32_t gy = std::max(config_.grid_y, 1u);
    const uint32_t x = index % gx;
    const uint32_t y = (index / gx) % gy;
    const uint32_t z = index / (gx * gy);
    const float3 pos = probe_position(x, y, z);

    float* sh = sh_.data() + static_cast<size_t>(index) * kSHFloatCount;
    std::memset(sh, 0, sizeof(float) * kSHFloatCount);

    const float3 sun_dir = normalize_or_zero(sun.direction);
    const float3 to_sun  = sun_dir * -1.0f;

    // ── 太陽可視 (バウンス項が使う。 delta 射影は include_direct 時のみ) ──
    const float sun_len = 1e6f;   // 平行光 — 実質無限遠
    const bool sun_visible =
        (sun.intensity > 0.0f) && !proxy_->occluded(pos, to_sun, sun_len);
    if (sun_visible && params_.include_direct) {
        const float3 radiance = sun.color * sun.intensity;
        sh_add_radiance(sh, to_sun, radiance, 1.0f);
    }

    // ── 点光源 (include_direct 時のみ。 可視レイ各 1 本、 距離減衰) ──────
    if (params_.include_direct) {
        for (const auto& light : points) {
            const float3 to_light = light.position - pos;
            const float dist = length3(to_light);
            if (dist < 1e-4f || dist > light.radius) continue;
            const float3 dir = to_light * (1.0f / dist);
            if (proxy_->occluded(pos, dir, dist)) continue;
            // 滑らかな半径減衰 (UE の inverse-square falloff 簡略形)。
            const float att = 1.0f - (dist / light.radius);
            const float3 radiance = light.color * (light.intensity * att * att);
            sh_add_radiance(sh, dir, radiance, 1.0f);
        }
    }

    // ── サンプル方向: 空 (ミス) + 1 次バウンス (ヒット) ────────────────
    const uint32_t rays = std::max(params_.rays_per_probe, 8u);
    const float* dist_cache = ray_hit_dist_.data()
                            + static_cast<size_t>(index) * rays;
    // モンテカルロ重み: 全球 4π を rays 本で等分。
    const float mc_weight = 4.0f * 3.14159265f / static_cast<float>(rays);

    const float3 sky = params_.sky_color * params_.sky_intensity;
    for (uint32_t r = 0; r < rays; ++r) {
        const float3 dir = fibonacci_sphere_dir(r, rays);
        const float hit_dist = dist_cache[r];
        if (hit_dist < 0.0f) {
            // 空 — 等方 sky 放射。
            sh_add_radiance(sh, dir, sky, mc_weight);
            continue;
        }
        // 1 次バウンス — ヒット面を Lambert (albedo 一律) とみなす。
        // ヒット点の太陽可視は probe 自身の可視で代用 (近似、 ヘッダ参照)。
        if (!sun_visible) continue;
        const float3 face_normal = dir * -1.0f;   // probe 向きの面と仮定
        const float ndotl = std::max(0.0f, dot3(face_normal, to_sun));
        if (ndotl <= 0.0f) continue;
        const float3 bounce = sun.color *
            (sun.intensity * ndotl * params_.bounce_albedo / 3.14159265f);
        sh_add_radiance(sh, dir, bounce, mc_weight);
    }

    // gi_intensity は最終スケール。
    if (config_.gi_intensity != 1.0f) {
        for (uint32_t c = 0; c < kSHFloatCount; ++c) {
            sh[c] *= config_.gi_intensity;
        }
    }
}

void GIProbeField::sample(const float3& pos, float out_sh[kSHFloatCount]) const {
    if (!built_) {
        std::memset(out_sh, 0, sizeof(float) * kSHFloatCount);
        return;
    }
    sample_probe_grid(config_, sh_.data(), pos, out_sh);
}

float3 GIProbeField::sample_irradiance(const float3& pos,
                                       const float3& normal) const {
    float sh[kSHFloatCount];
    sample(pos, sh);
    return sh_eval_irradiance(sh, normal);
}

} // namespace pictor
