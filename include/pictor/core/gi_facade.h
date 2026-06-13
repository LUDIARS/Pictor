#pragma once

#include "pictor/gi/gi_lighting_system.h"
#include "pictor/gi/gi_bake.h"
#include <cstdint>
#include <string>

namespace pictor {

class RendererSubsystemManager;

/// GI ライティング + 静的ベイクの委譲ファサード
/// (review/2026-06-11 D-1: PictorRenderer God Class からの切り出し)。
///
/// gi_system / bake_system の所有は RendererSubsystemManager 側にあり (profile 切替で
/// 生成・破棄され得る) 、 本ファサードは毎回 manager から live に取得して委譲する
/// (stale ポインタを持たない)。 bake_timestamp 用に現フレーム番号を参照する。
class GIFacade {
public:
    GIFacade(RendererSubsystemManager& subsystems, const uint64_t& frame_number)
        : subsystems_(subsystems), frame_number_(frame_number) {}

    // ---- GI Lighting ----
    void set_directional_light(const DirectionalLight& light);
    void upload_gi_probe_data(const float* sh_data, uint32_t probe_count);
    void set_gi_config(const GIConfig& config);

    // ---- GI Bake (static objects) ----
    GIBakeResult bake_static_gi();
    GIBakeResult bake_static_gi(BakeProgressCallback progress);
    void apply_bake(const GIBakeResult& result);
    void invalidate_bake();
    bool save_bake(const std::string& path, const GIBakeResult& result);
    GIBakeResult load_bake(const std::string& path);
    void set_bake_data_provider(IBakeDataProvider* provider);

private:
    RendererSubsystemManager& subsystems_;
    const uint64_t&           frame_number_;
};

} // namespace pictor
