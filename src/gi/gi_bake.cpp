#include "pictor/gi/gi_bake.h"
#include "pictor/gi/gi_sh.h"
#include <cmath>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <cstring>

namespace pictor {

namespace {

float3 normalize_or_zero(const float3& v) {
    const float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
    if (len2 < 1e-12f) return {0.0f, 0.0f, 0.0f};
    return v * (1.0f / std::sqrt(len2));
}

float length3(const float3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

/// オブジェクトの太陽可視率 — AABB 中心 + 上面 4 隅の 5 レイの平均。
/// 面ではなく代表点のサンプルなので 0/0.2/…/1.0 の離散値になる
/// (per-object GI の粒度としては十分 — spec §2)。
float sun_visibility(const GISceneProxy& proxy, const AABB& box,
                     ObjectId self, const float3& to_sun) {
    const float3 c = box.center();
    const float3 samples[5] = {
        c,
        {box.min.x, box.max.y, box.min.z},
        {box.max.x, box.max.y, box.min.z},
        {box.min.x, box.max.y, box.max.z},
        {box.max.x, box.max.y, box.max.z},
    };
    constexpr float kSunRayLen = 1e6f;   // 平行光 — 実質無限遠
    int lit = 0;
    for (const auto& s : samples) {
        if (!proxy.occluded(s, to_sun, kSunRayLen, self)) ++lit;
    }
    return static_cast<float>(lit) / 5.0f;
}

} // namespace

GIBakeSystem::GIBakeSystem(GPUBufferManager& buffer_manager,
                           SceneRegistry& registry,
                           GILightingSystem& gi_system)
    : buffer_manager_(buffer_manager)
    , registry_(registry)
    , gi_system_(gi_system)
{
}

GIBakeSystem::~GIBakeSystem() = default;

// ============================================================
// Bake Entry Points
// ============================================================

GIBakeResult GIBakeSystem::bake() {
    return bake(BakeProgressCallback{});
}

GIBakeResult GIBakeSystem::bake(BakeProgressCallback progress) {
    auto start = std::chrono::high_resolution_clock::now();

    GIBakeResult result;
    stats_ = {};

    const ObjectPool& static_pool = registry_.static_pool();
    uint32_t count = static_pool.count();

    if (count == 0) {
        result.valid = true;
        baked_ = true;
        dirty_ = false;
        return result;
    }

    // Notify provider
    if (provider_) {
        provider_->on_bake_begin();
    }

    // Collect static object IDs
    collect_static_objects(static_pool, result);

    // Build the shared bake scene (occlusion proxy / probe field / lights)
    prepare_bake_scene(static_pool);

    // Count active targets for progress tracking
    uint32_t total_passes = 0;
    if (has_flag(config_.targets, BakeTarget::SHADOW_MAP))       ++total_passes;
    if (has_flag(config_.targets, BakeTarget::AMBIENT_OCCLUSION)) ++total_passes;
    if (has_flag(config_.targets, BakeTarget::PROBE_IRRADIANCE)) ++total_passes;
    if (has_flag(config_.targets, BakeTarget::LIGHTMAP))         ++total_passes;

    uint32_t current_pass = 0;

    // Pass 1: Shadow bake
    if (has_flag(config_.targets, BakeTarget::SHADOW_MAP)) {
        if (progress && !progress(
                static_cast<float>(current_pass) / total_passes, "Baking shadows")) {
            return result; // cancelled
        }
        if (!bake_shadows(static_pool, result, progress)) return result;
        ++current_pass;
        ++stats_.bake_passes;
    }

    // Pass 2: AO bake
    if (has_flag(config_.targets, BakeTarget::AMBIENT_OCCLUSION)) {
        if (progress && !progress(
                static_cast<float>(current_pass) / total_passes, "Baking ambient occlusion")) {
            return result;
        }
        if (!bake_ao(static_pool, result, progress)) return result;
        ++current_pass;
        ++stats_.bake_passes;
    }

    // Pass 3: Irradiance bake
    if (has_flag(config_.targets, BakeTarget::PROBE_IRRADIANCE)) {
        if (progress && !progress(
                static_cast<float>(current_pass) / total_passes, "Baking probe irradiance")) {
            return result;
        }
        if (!bake_irradiance(static_pool, result, progress)) return result;
        ++current_pass;
        ++stats_.bake_passes;
    }

    // Pass 4: Lightmap bake
    if (has_flag(config_.targets, BakeTarget::LIGHTMAP)) {
        if (progress && !progress(
                static_cast<float>(current_pass) / total_passes, "Baking lightmaps")) {
            return result;
        }
        if (!bake_lightmap(static_pool, result, progress)) return result;
        ++current_pass;
        ++stats_.bake_passes;
    }

    auto end = std::chrono::high_resolution_clock::now();
    stats_.bake_time_ms = std::chrono::duration<float, std::milli>(end - start).count();
    stats_.baked_objects = count;

    result.valid = true;
    baked_ = true;
    dirty_ = false;

    if (progress) {
        progress(1.0f, "Bake complete");
    }

    // Notify provider
    if (provider_) {
        provider_->on_bake_complete(result);
    }

    return result;
}

// ============================================================
// Apply — upload baked data to GPU
// ============================================================

void GIBakeSystem::apply(const GIBakeResult& result) {
    if (!result.valid || result.object_ids.empty()) return;

    uint32_t count = static_cast<uint32_t>(result.object_ids.size());

    // Allocate GPU buffers for baked data (persistent, not per-frame)

    if (!result.shadows.empty()) {
        baked_shadow_flags_ =
            buffer_manager_.allocate_instance_data(count * sizeof(uint32_t));
        baked_shadow_depths_ =
            buffer_manager_.allocate_instance_data(count * 4 * sizeof(float));

        // In real Vulkan: staging upload of shadow_cascade_flags and shadow_depths
        // The data would be copied from result.shadows[i].cascade_flags / depths
    }

    if (!result.ao.empty()) {
        baked_ao_ =
            buffer_manager_.allocate_instance_data(count * sizeof(float));

        // In real Vulkan: staging upload of per-object AO values
    }

    if (!result.irradiance.empty()) {
        baked_irradiance_ =
            buffer_manager_.allocate_instance_data(count * 9 * 4 * sizeof(float));

        // In real Vulkan: staging upload of SH coefficients
    }

    if (!result.lightmaps.empty()) {
        baked_lightmap_ =
            buffer_manager_.allocate_instance_data(count * sizeof(BakedLightmap));

        // In real Vulkan: staging upload of lightmap data
    }
}

// ============================================================
// Invalidate
// ============================================================

void GIBakeSystem::invalidate() {
    dirty_ = true;
}

// ============================================================
// Bake Scene Preparation
// ============================================================

void GIBakeSystem::prepare_bake_scene(const ObjectPool& pool) {
    bake_proxy_.build(pool);

    bake_point_lights_.clear();
    if (provider_) {
        bake_point_lights_ = provider_->get_bake_point_lights();
    }

    // probe field は irradiance / lightmap のどちらかが対象のときだけ構築
    // (probe 数 × レイ数のコストを他ターゲットに払わせない)。
    if (has_flag(config_.targets, BakeTarget::PROBE_IRRADIANCE) ||
        has_flag(config_.targets, BakeTarget::LIGHTMAP)) {
        GIProbeField::BuildParams params;
        // bake は realtime 既定 (64) より高密度にサンプルする。
        params.rays_per_probe =
            std::max(64u, config_.lightmap.samples_per_texel);
        bake_probes_.build(config_.probes, bake_proxy_, light_,
                           bake_point_lights_, params);
    }
}

// ============================================================
// Shadow Bake
// ============================================================

bool GIBakeSystem::bake_shadows(const ObjectPool& pool, GIBakeResult& result,
                                const BakeProgressCallback& progress) {
    const uint32_t count = pool.count();
    result.shadows.resize(count);

    const uint32_t cascade_count = std::min(config_.shadow.cascade_count, 4u);
    const float3 to_sun = normalize_or_zero(light_.direction) * -1.0f;

    const auto& bounds = pool.bounds();
    const auto& ids    = pool.object_ids();

    for (uint32_t i = 0; i < count; i++) {
        // cascade 割当はカメラ依存 — ベイクでは決められないため全 cascade を
        // 立てる (ランタイムのカリングが絞る)。 depths には静的な太陽可視率
        // (soft shadow factor) を焼く。
        result.shadows[i].cascade_flags = (1u << cascade_count) - 1;

        const float lit = (light_.intensity > 0.0f)
            ? sun_visibility(bake_proxy_, bounds[i], ids[i], to_sun)
            : 1.0f;
        const float shadowed =
            1.0f - (1.0f - lit) * config_.shadow.shadow_strength;
        for (uint32_t c = 0; c < 4; c++) {
            result.shadows[i].depths[c] =
                (c < cascade_count) ? shadowed : 1.0f;
        }

        if (progress && (i % 256u) == 0u &&
            !progress(static_cast<float>(i) / static_cast<float>(count),
                      "Baking shadows")) {
            return false;
        }
    }
    return true;
}

// ============================================================
// AO Bake
// ============================================================

bool GIBakeSystem::bake_ao(const ObjectPool& pool, GIBakeResult& result,
                           const BakeProgressCallback& progress) {
    const uint32_t count = pool.count();
    result.ao.resize(count);

    // object-space AO — 各オブジェクトの AABB 中心から fibonacci sphere の
    // 全球方向へレイを撃ち、 radius 以内の遮蔽率を距離減衰付きで積分する。
    // realtime SSAO (screen-space) より高サンプル (既定 256)。
    const uint32_t samples = std::max(config_.ao.sample_count, 16u);
    const float    radius  = std::max(config_.ao.radius, 1e-3f);

    const auto& bounds = pool.bounds();
    const auto& ids    = pool.object_ids();

    for (uint32_t i = 0; i < count; i++) {
        const float3 center = bounds[i].center();
        float occlusion_sum = 0.0f;

        for (uint32_t s = 0; s < samples; ++s) {
            const float3 dir = fibonacci_sphere_dir(s, samples);
            const auto hit = bake_proxy_.closest_hit(center, dir, radius, ids[i]);
            if (!hit.hit || hit.distance < config_.ao.bias) continue;
            // 近い遮蔽ほど強く効かせる線形減衰。
            occlusion_sum += 1.0f - hit.distance / radius;
        }

        const float occluded_fraction =
            occlusion_sum / static_cast<float>(samples);
        result.ao[i].occlusion = std::clamp(
            1.0f - occluded_fraction * config_.ao.intensity, 0.0f, 1.0f);

        if (progress && (i % 64u) == 0u &&
            !progress(static_cast<float>(i) / static_cast<float>(count),
                      "Baking ambient occlusion")) {
            return false;
        }
    }
    return true;
}

// ============================================================
// Irradiance Bake
// ============================================================

bool GIBakeSystem::bake_irradiance(const ObjectPool& pool, GIBakeResult& result,
                                   const BakeProgressCallback& progress) {
    const uint32_t count = pool.count();
    result.irradiance.resize(count);

    // probe grid (prepare_bake_scene で構築済み) をオブジェクト位置で
    // trilinear 補間して per-object SH をキャッシュする。 ランタイムの
    // gi_probe_sample.comp と同じ演算の CPU 版。
    const auto& bounds = pool.bounds();

    for (uint32_t i = 0; i < count; i++) {
        bake_probes_.sample(bounds[i].center(), result.irradiance[i].sh);

        if (progress && (i % 256u) == 0u &&
            !progress(static_cast<float>(i) / static_cast<float>(count),
                      "Baking probe irradiance")) {
            return false;
        }
    }
    return true;
}

// ============================================================
// Lightmap Bake
// ============================================================

bool GIBakeSystem::bake_lightmap(const ObjectPool& pool, GIBakeResult& result,
                                 const BakeProgressCallback& progress) {
    const uint32_t count = pool.count();
    result.lightmaps.resize(count);

    // per-object lightmap — 上向き法線の代表面として direct / indirect を焼く
    // (per-texel ライトマップは目標にしない — spec §2)。
    //   direct   : 太陽 (可視率 × N·L) + 点光源 (可視 × 距離減衰 × N·L)
    //   indirect : probe field の irradiance × indirect_intensity ×
    //              バウンス増幅 (probe は 1 次バウンスまで —
    //              2 次以降は albedo^k の等比和で近似する)
    const float3 up     = {0.0f, 1.0f, 0.0f};
    const float3 to_sun = normalize_or_zero(light_.direction) * -1.0f;
    const float  sun_ndotl = std::max(0.0f, to_sun.y);   // dot(up, to_sun)

    const uint32_t bounces = std::max(config_.lightmap.bounce_count, 1u);
    constexpr float kBounceAlbedo = 0.5f;   // GIProbeField 既定と揃える
    float bounce_gain = 0.0f;
    for (uint32_t k = 0; k < bounces; ++k) {
        bounce_gain += std::pow(kBounceAlbedo, static_cast<float>(k));
    }

    const auto& bounds = pool.bounds();
    const auto& ids    = pool.object_ids();

    for (uint32_t i = 0; i < count; i++) {
        const AABB&  box    = bounds[i];
        const float3 center = box.center();
        auto& lm = result.lightmaps[i];

        // ── direct: 太陽 ────────────────────────────────────────────
        float3 direct{};
        if (light_.intensity > 0.0f && sun_ndotl > 0.0f) {
            const float lit = sun_visibility(bake_proxy_, box, ids[i], to_sun);
            const float scale = light_.intensity * sun_ndotl * lit;
            direct = light_.color * scale;
        }

        // ── direct: 点光源 (provider 供給) ──────────────────────────
        for (const auto& pl : bake_point_lights_) {
            const float3 to_light = pl.position - center;
            const float dist = length3(to_light);
            if (dist < 1e-4f || dist > pl.radius) continue;
            const float3 dir = to_light * (1.0f / dist);
            if (bake_proxy_.occluded(center, dir, dist, ids[i])) continue;
            const float att   = 1.0f - dist / pl.radius;
            const float ndotl = std::max(0.0f, dir.y);   // dot(up, dir)
            direct = direct + pl.color * (pl.intensity * att * att * ndotl);
        }

        lm.direct_r = direct.x;
        lm.direct_g = direct.y;
        lm.direct_b = direct.z;

        // ── indirect: probe field ───────────────────────────────────
        const float3 indirect = bake_probes_.sample_irradiance(center, up);
        const float gain = config_.lightmap.indirect_intensity * bounce_gain;
        lm.indirect_r = indirect.x * gain;
        lm.indirect_g = indirect.y * gain;
        lm.indirect_b = indirect.z * gain;

        if (progress && (i % 64u) == 0u &&
            !progress(static_cast<float>(i) / static_cast<float>(count),
                      "Baking lightmaps")) {
            return false;
        }
    }
    return true;
}

// ============================================================
// Collect Static Objects
// ============================================================

void GIBakeSystem::collect_static_objects(const ObjectPool& pool, GIBakeResult& result) {
    uint32_t count = pool.count();
    result.object_ids.resize(count);

    const auto& ids = pool.object_ids();
    for (uint32_t i = 0; i < count; i++) {
        result.object_ids[i] = ids[i];
    }
}

// ============================================================
// Serialization
// ============================================================

namespace {
    constexpr uint32_t BAKE_MAGIC   = 0x50494342; // "PICB"
    constexpr uint32_t BAKE_VERSION = 1;

    struct BakeFileHeader {
        uint32_t magic;
        uint32_t version;
        uint32_t object_count;
        uint32_t flags;          // bitmask of which data sections are present
        uint64_t timestamp;
    };
}

bool GIBakeSystem::save(const std::string& path, const GIBakeResult& result) const {
    if (!result.valid) return false;

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    BakeFileHeader header;
    header.magic = BAKE_MAGIC;
    header.version = BAKE_VERSION;
    header.object_count = static_cast<uint32_t>(result.object_ids.size());
    header.flags = 0;
    if (!result.shadows.empty())    header.flags |= static_cast<uint32_t>(BakeTarget::SHADOW_MAP);
    if (!result.ao.empty())         header.flags |= static_cast<uint32_t>(BakeTarget::AMBIENT_OCCLUSION);
    if (!result.irradiance.empty()) header.flags |= static_cast<uint32_t>(BakeTarget::PROBE_IRRADIANCE);
    if (!result.lightmaps.empty())  header.flags |= static_cast<uint32_t>(BakeTarget::LIGHTMAP);
    header.timestamp = result.bake_timestamp;

    file.write(reinterpret_cast<const char*>(&header), sizeof(header));

    // Object IDs
    file.write(reinterpret_cast<const char*>(result.object_ids.data()),
               result.object_ids.size() * sizeof(ObjectId));

    // Shadows
    if (!result.shadows.empty()) {
        file.write(reinterpret_cast<const char*>(result.shadows.data()),
                   result.shadows.size() * sizeof(BakedShadow));
    }

    // AO
    if (!result.ao.empty()) {
        file.write(reinterpret_cast<const char*>(result.ao.data()),
                   result.ao.size() * sizeof(BakedAO));
    }

    // Irradiance
    if (!result.irradiance.empty()) {
        file.write(reinterpret_cast<const char*>(result.irradiance.data()),
                   result.irradiance.size() * sizeof(BakedIrradiance));
    }

    // Lightmaps
    if (!result.lightmaps.empty()) {
        file.write(reinterpret_cast<const char*>(result.lightmaps.data()),
                   result.lightmaps.size() * sizeof(BakedLightmap));
    }

    return file.good();
}

GIBakeResult GIBakeSystem::load(const std::string& path) const {
    GIBakeResult result;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return result;

    BakeFileHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (header.magic != BAKE_MAGIC || header.version != BAKE_VERSION) {
        return result;
    }

    uint32_t count = header.object_count;
    result.bake_timestamp = header.timestamp;

    // Object IDs
    result.object_ids.resize(count);
    file.read(reinterpret_cast<char*>(result.object_ids.data()),
              count * sizeof(ObjectId));

    // Shadows
    if (header.flags & static_cast<uint32_t>(BakeTarget::SHADOW_MAP)) {
        result.shadows.resize(count);
        file.read(reinterpret_cast<char*>(result.shadows.data()),
                  count * sizeof(BakedShadow));
    }

    // AO
    if (header.flags & static_cast<uint32_t>(BakeTarget::AMBIENT_OCCLUSION)) {
        result.ao.resize(count);
        file.read(reinterpret_cast<char*>(result.ao.data()),
                  count * sizeof(BakedAO));
    }

    // Irradiance
    if (header.flags & static_cast<uint32_t>(BakeTarget::PROBE_IRRADIANCE)) {
        result.irradiance.resize(count);
        file.read(reinterpret_cast<char*>(result.irradiance.data()),
                  count * sizeof(BakedIrradiance));
    }

    // Lightmaps
    if (header.flags & static_cast<uint32_t>(BakeTarget::LIGHTMAP)) {
        result.lightmaps.resize(count);
        file.read(reinterpret_cast<char*>(result.lightmaps.data()),
                  count * sizeof(BakedLightmap));
    }

    result.valid = file.good();
    return result;
}

uint32_t GIBakeSystem::calculate_workgroups(uint32_t count) const {
    return (count + config_.workgroup_size - 1) / config_.workgroup_size;
}

} // namespace pictor
