#include "pictor/gi/gi_bake.h"
#include <cmath>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <cstring>

namespace pictor {

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
    return bake(nullptr);
}

GIBakeResult GIBakeSystem::bake(BakeProgressCallback progress) {
    return bake(&progress);
}

GIBakeResult GIBakeSystem::bake(BakeProgressCallback* progress) {
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

    // Count active targets for progress tracking
    uint32_t total_passes = 0;
    if (has_flag(config_.targets, BakeTarget::SHADOW_MAP))       ++total_passes;
    if (has_flag(config_.targets, BakeTarget::AMBIENT_OCCLUSION)) ++total_passes;
    if (has_flag(config_.targets, BakeTarget::PROBE_IRRADIANCE)) ++total_passes;
    if (has_flag(config_.targets, BakeTarget::LIGHTMAP))         ++total_passes;

    uint32_t current_pass = 0;

    // Pass 1: Shadow bake
    if (has_flag(config_.targets, BakeTarget::SHADOW_MAP)) {
        if (progress && !(*progress)(
                static_cast<float>(current_pass) / total_passes, "Baking shadows")) {
            return result; // cancelled
        }
        bake_shadows(static_pool, result, progress);
        ++current_pass;
        ++stats_.bake_passes;
    }

    // Pass 2: AO bake
    if (has_flag(config_.targets, BakeTarget::AMBIENT_OCCLUSION)) {
        if (progress && !(*progress)(
                static_cast<float>(current_pass) / total_passes, "Baking ambient occlusion")) {
            return result;
        }
        bake_ao(static_pool, result, progress);
        ++current_pass;
        ++stats_.bake_passes;
    }

    // Pass 3: Irradiance bake
    if (has_flag(config_.targets, BakeTarget::PROBE_IRRADIANCE)) {
        if (progress && !(*progress)(
                static_cast<float>(current_pass) / total_passes, "Baking probe irradiance")) {
            return result;
        }
        bake_irradiance(static_pool, result, progress);
        ++current_pass;
        ++stats_.bake_passes;
    }

    // Pass 4: Lightmap bake
    if (has_flag(config_.targets, BakeTarget::LIGHTMAP)) {
        if (progress && !(*progress)(
                static_cast<float>(current_pass) / total_passes, "Baking lightmaps")) {
            return result;
        }
        bake_lightmap(static_pool, result, progress);
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
        (*progress)(1.0f, "Bake complete");
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
// Shadow Bake
// ============================================================

void GIBakeSystem::bake_shadows(const ObjectPool& pool, GIBakeResult& result,
                                BakeProgressCallback* progress) {
    uint32_t count = pool.count();
    result.shadows.resize(count);

    // Compute cascade splits (same algorithm as runtime)
    uint32_t cascade_count = std::min(config_.shadow.cascade_count, 4u);

    // For static bake, we compute shadow data against a fixed light direction.
    // This is a one-time compute dispatch that writes per-object cascade flags + depths.
    //
    // In real Vulkan:
    //   1. Compute light-space VP matrices for each cascade
    //   2. Upload static pool bounds/transforms to input SSBOs
    //   3. Dispatch shadow_map_gen.comp against static objects only
    //   4. Readback results to CPU for caching

    uint32_t workgroups = calculate_workgroups(count);
    stats_.total_workgroups += workgroups;

    // Placeholder: assign cascade flags based on AABB position relative to light
    const auto& bounds = pool.bounds();
    const auto& transforms = pool.transforms();

    for (uint32_t i = 0; i < count; i++) {
        const AABB& aabb = bounds[i];
        float3 center = aabb.center();

        // Simplified: all static objects are shadow casters in cascade 0
        // Real implementation uses the compute shader for accurate cascade assignment
        result.shadows[i].cascade_flags = (1u << cascade_count) - 1; // all cascades
        for (uint32_t c = 0; c < cascade_count; c++) {
            result.shadows[i].depths[c] = 0.5f; // placeholder depth
        }
    }

    (void)transforms;
    (void)progress;
}

// ============================================================
// AO Bake
// ============================================================

void GIBakeSystem::bake_ao(const ObjectPool& pool, GIBakeResult& result,
                           BakeProgressCallback* progress) {
    uint32_t count = pool.count();
    result.ao.resize(count);

    // For static AO bake, we use a much higher sample count than realtime SSAO.
    // Instead of screen-space, we compute object-space AO by casting rays from
    // each object's position against surrounding static geometry.
    //
    // In real Vulkan:
    //   1. Upload static pool bounds/transforms
    //   2. Upload AO sample kernel (config_.ao.sample_count directions)
    //   3. Dispatch static_ao_bake.comp with all static objects
    //   4. Readback per-object AO values

    uint32_t workgroups = calculate_workgroups(count);
    stats_.total_workgroups += workgroups;

    const auto& bounds = pool.bounds();

    for (uint32_t i = 0; i < count; i++) {
        // Placeholder: estimate AO based on nearby object density
        // Real implementation uses the compute shader with ray-AABB intersection
        result.ao[i].occlusion = 1.0f;
    }

    (void)bounds;
    (void)progress;
}

// ============================================================
// Irradiance Bake
// ============================================================

void GIBakeSystem::bake_irradiance(const ObjectPool& pool, GIBakeResult& result,
                                   BakeProgressCallback* progress) {
    uint32_t count = pool.count();
    result.irradiance.resize(count);

    // For static irradiance bake, we interpolate probe data once and cache it.
    // This is identical to the runtime gi_probe_sample.comp pass, but we
    // store the result permanently.
    //
    // In real Vulkan:
    //   1. Ensure probe irradiance SSBO has valid data
    //   2. Upload static transforms
    //   3. Dispatch gi_probe_sample.comp for static objects
    //   4. Readback per-object SH data

    uint32_t workgroups = calculate_workgroups(count);
    stats_.total_workgroups += workgroups;

    const auto& transforms = pool.transforms();

    for (uint32_t i = 0; i < count; i++) {
        // Placeholder: zero irradiance (probe data would be interpolated by shader)
        std::memset(result.irradiance[i].sh, 0, sizeof(result.irradiance[i].sh));
    }

    (void)transforms;
    (void)progress;
}

// ============================================================
// Lightmap Bake
// ============================================================

void GIBakeSystem::bake_lightmap(const ObjectPool& pool, GIBakeResult& result,
                                 BakeProgressCallback* progress) {
    uint32_t count = pool.count();
    result.lightmaps.resize(count);

    // Lightmap bake combines direct and indirect illumination.
    // For each static object:
    //   1. Compute direct lighting from directional + point lights
    //   2. Compute indirect lighting via multiple bounces
    //   3. Store combined result
    //
    // In real Vulkan:
    //   1. Upload static scene data (bounds, transforms, materials)
    //   2. Upload light sources (directional + point lights from provider)
    //   3. For each bounce: dispatch lightmap_bake.comp
    //   4. Accumulate results across bounces
    //   5. Readback combined lightmap data

    std::vector<PointLight> extra_lights;
    if (provider_) {
        extra_lights = provider_->get_bake_point_lights();
    }

    uint32_t bounces = config_.lightmap.bounce_count;
    uint32_t workgroups = calculate_workgroups(count);
    stats_.total_workgroups += workgroups * (bounces + 1); // direct + N bounces

    const auto& transforms = pool.transforms();
    const auto& bounds = pool.bounds();

    for (uint32_t i = 0; i < count; i++) {
        float3 center = bounds[i].center();

        // Placeholder: simple N·L directional lighting
        float3 normal = {0.0f, 1.0f, 0.0f}; // assume upward normal
        float ndotl = -(light_.direction.x * normal.x +
                        light_.direction.y * normal.y +
                        light_.direction.z * normal.z);
        ndotl = std::max(0.0f, ndotl);

        result.lightmaps[i].direct_r = light_.color.x * light_.intensity * ndotl;
        result.lightmaps[i].direct_g = light_.color.y * light_.intensity * ndotl;
        result.lightmaps[i].direct_b = light_.color.z * light_.intensity * ndotl;

        // Indirect: placeholder uniform ambient
        result.lightmaps[i].indirect_r = 0.1f;
        result.lightmaps[i].indirect_g = 0.1f;
        result.lightmaps[i].indirect_b = 0.1f;
    }

    (void)transforms;
    (void)extra_lights;
    (void)progress;
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
