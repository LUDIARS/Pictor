#pragma once

#include "pictor/xr/delta_render_planner.h"
#include <cstdint>
#include <string_view>

namespace pictor::xr {

/// 名前で選べる両眼描画の既定値 (PC-RULE-003)。 demo と host が同じ入口から選び、
/// 返ってきた config を上書きして使う。 ID と版は安定させる。
struct StereoPreset {
    std::string_view  id;
    uint32_t          version = 1;
    std::string_view  description;
    DeltaRenderConfig config;
    /// true の preset は VK_KHR_multiview が有効なデバイスでしか使えない。
    bool              requires_multiview = false;
};

/// 見つからなければ nullptr。 呼び出し側は明示エラーにすること
/// (別の preset へ黙って置き換えない)。
const StereoPreset* find_stereo_preset(std::string_view id);

uint32_t            stereo_preset_count();
const StereoPreset& stereo_preset_at(uint32_t index);

} // namespace pictor::xr
