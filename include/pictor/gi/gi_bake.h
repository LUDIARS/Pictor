#pragma once

#include "pictor/core/types.h"
#include "pictor/gpu/gpu_buffer_manager.h"
#include "pictor/scene/scene_registry.h"
#include "pictor/gi/gi_lighting_system.h"
#include <vector>
#include <string>
#include <functional>

namespace pictor {

// ============================================================
// Bake Targets — what to bake for static objects
// ============================================================

enum class BakeTarget : uint8_t {
    SHADOW_MAP       = 1 << 0,  // Static shadow depth + cascade flags
    AMBIENT_OCCLUSION = 1 << 1, // Per-object AO (not screen-space — object-space)
    PROBE_IRRADIANCE = 1 << 2,  // Per-object irradiance from probe grid
    LIGHTMAP         = 1 << 3,  // Combined direct+indirect lighting per object
    ALL              = 0x0F
};

inline BakeTarget operator|(BakeTarget a, BakeTarget b) {
    return static_cast<BakeTarget>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline BakeTarget operator&(BakeTarget a, BakeTarget b) {
    return static_cast<BakeTarget>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}
inline bool has_flag(BakeTarget mask, BakeTarget flag) {
    return (static_cast<uint8_t>(mask) & static_cast<uint8_t>(flag)) != 0;
}

// ============================================================
// Bake Configuration
// ============================================================

/// AO bake settings (higher quality than realtime SSAO)
struct BakeAOConfig {
    uint32_t sample_count     = 256;   // much higher than realtime
    float    radius           = 1.0f;
    float    bias             = 0.01f;
    float    intensity        = 1.0f;
};

/// Lightmap bake settings
struct BakeLightmapConfig {
    uint32_t bounce_count     = 3;     // indirect light bounces
    uint32_t samples_per_texel = 64;   // rays per lightmap texel
    uint32_t lightmap_resolution = 128; // per-object texel resolution
    float    indirect_intensity = 1.0f;
};

struct GIBakeConfig {
    BakeTarget          targets    = BakeTarget::ALL;
    ShadowMapConfig     shadow;     // reuse GI shadow config
    BakeAOConfig        ao;
    GIProbeConfig       probes;     // reuse GI probe config
    BakeLightmapConfig  lightmap;
    uint32_t            workgroup_size = 256;
};

// ============================================================
// Bake Result — per-object baked data on CPU
// ============================================================

/// Per-object baked shadow data
struct BakedShadow {
    uint32_t cascade_flags = 0;     // bitmask of cascades this object touches
    float    depths[4]     = {1.0f, 1.0f, 1.0f, 1.0f};
};

/// Per-object baked AO
struct BakedAO {
    float occlusion = 1.0f;         // 0 = fully occluded, 1 = fully open
};

/// Per-object baked irradiance (L2 SH)
struct BakedIrradiance {
    float sh[9 * 4] = {};           // 9 × vec4 = RGB SH coefficients
};

/// Per-object baked lightmap (combined direct + indirect)
struct BakedLightmap {
    float direct_r  = 0.0f, direct_g  = 0.0f, direct_b  = 0.0f;
    float indirect_r = 0.0f, indirect_g = 0.0f, indirect_b = 0.0f;
};

/// Complete bake result for the static scene
struct GIBakeResult {
    std::vector<ObjectId>        object_ids;
    std::vector<BakedShadow>     shadows;
    std::vector<BakedAO>         ao;
    std::vector<BakedIrradiance> irradiance;
    std::vector<BakedLightmap>   lightmaps;
    uint64_t                     bake_timestamp = 0;  // frame number at bake time
    bool                         valid = false;
};

// ============================================================
// Bake Progress Callback
// ============================================================

/// Reports bake progress. Return false to cancel the bake.
using BakeProgressCallback = std::function<bool(float progress, const char* stage)>;

// ============================================================
// IBakeDataProvider — user-supplied bake input
// ============================================================

/// Optional interface for users to supply additional bake input
/// (e.g. custom light sources, emissive surfaces, etc.)
class IBakeDataProvider {
public:
    virtual ~IBakeDataProvider() = default;

    /// Provide additional point lights for the bake
    virtual std::vector<PointLight> get_bake_point_lights() const { return {}; }

    /// Provide emissive surface data (object IDs that emit light)
    virtual std::vector<ObjectId> get_emissive_objects() const { return {}; }

    /// Called before bake starts — chance to prepare data
    virtual void on_bake_begin() {}

    /// Called after bake completes
    virtual void on_bake_complete(const GIBakeResult& result) { (void)result; }
};

// ============================================================
// GIBakeSystem
// ============================================================

/// Offline / pre-render bake system for static objects.
///
/// Generates high-quality GI data for objects in the Static pool:
///   - Shadow cascade assignment + depths
///   - Per-object ambient occlusion
///   - Per-object irradiance from probe grid
///   - Combined lightmap (direct + indirect)
///
/// Baked results are cached and uploaded to GPU SSBOs so that
/// runtime GI passes can skip static objects entirely.
///
/// Usage:
///   GIBakeSystem baker(buffer_manager, registry, gi_system);
///   baker.set_config(bake_config);
///   baker.set_directional_light(sun);
///   auto result = baker.bake();           // blocking
///   baker.bake_async(callback);           // non-blocking
///   baker.apply(result);                  // upload to GPU
///   baker.invalidate();                   // mark stale after scene change
///
class GIBakeSystem {
public:
    GIBakeSystem(GPUBufferManager& buffer_manager,
                 SceneRegistry& registry,
                 GILightingSystem& gi_system);
    ~GIBakeSystem();

    GIBakeSystem(const GIBakeSystem&) = delete;
    GIBakeSystem& operator=(const GIBakeSystem&) = delete;

    // ---- Configuration ----

    void set_config(const GIBakeConfig& config) { config_ = config; }
    const GIBakeConfig& config() const { return config_; }

    void set_directional_light(const DirectionalLight& light) { light_ = light; }
    void set_bake_data_provider(IBakeDataProvider* provider) { provider_ = provider; }

    // ---- Bake Operations ----

    /// Bake GI data for all static objects. Blocking call.
    /// Returns the bake result which can be applied or serialized.
    GIBakeResult bake();

    /// Bake with progress reporting. Return false from callback to cancel.
    GIBakeResult bake(BakeProgressCallback progress);

    /// Apply baked results to GPU resources for runtime use.
    /// After apply(), runtime GI passes will read baked data for static objects.
    void apply(const GIBakeResult& result);

    /// Mark baked data as stale (e.g. after static object added/removed/moved).
    /// Does not clear existing data — it remains usable until next bake().
    void invalidate();

    /// Check if current bake is still valid
    bool is_valid() const { return baked_ && !dirty_; }

    /// Check if a bake has been applied
    bool is_baked() const { return baked_; }

    /// Check if bake is stale (scene changed since last bake)
    bool is_dirty() const { return dirty_; }

    // ---- Serialization ----

    /// Save bake result to a binary file for fast reload
    bool save(const std::string& path, const GIBakeResult& result) const;

    /// Load bake result from a binary file
    GIBakeResult load(const std::string& path) const;

    // ---- Statistics ----

    struct Stats {
        uint32_t baked_objects    = 0;
        uint32_t bake_passes     = 0;
        uint32_t total_workgroups = 0;
        float    bake_time_ms    = 0.0f;
    };

    Stats get_stats() const { return stats_; }

private:
    /// Bake shadow data for static objects
    void bake_shadows(const ObjectPool& pool, GIBakeResult& result,
                      const BakeProgressCallback& progress);

    /// Bake per-object AO for static objects
    void bake_ao(const ObjectPool& pool, GIBakeResult& result,
                 const BakeProgressCallback& progress);

    /// Bake irradiance from probe grid
    void bake_irradiance(const ObjectPool& pool, GIBakeResult& result,
                         const BakeProgressCallback& progress);

    /// Bake combined lightmap
    void bake_lightmap(const ObjectPool& pool, GIBakeResult& result,
                       const BakeProgressCallback& progress);

    /// Collect static object IDs into the result
    void collect_static_objects(const ObjectPool& pool, GIBakeResult& result);

    uint32_t calculate_workgroups(uint32_t count) const;

    GPUBufferManager&  buffer_manager_;
    SceneRegistry&     registry_;
    GILightingSystem&  gi_system_;
    GIBakeConfig       config_;
    DirectionalLight   light_;
    IBakeDataProvider* provider_ = nullptr;
    Stats              stats_;
    bool               baked_ = false;
    bool               dirty_ = false;

    // GPU resources for baked data
    GpuAllocation baked_shadow_flags_;
    GpuAllocation baked_shadow_depths_;
    GpuAllocation baked_ao_;
    GpuAllocation baked_irradiance_;
    GpuAllocation baked_lightmap_;
};

} // namespace pictor
