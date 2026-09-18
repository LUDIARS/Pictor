#include "pictor/xr/stereo_presets.h"

namespace pictor::xr {

namespace {

/// @implements SPEC-PC-XR-STEREO-PRESETS
DeltaRenderConfig make_config(StereoMode mode, bool temporal) {
    DeltaRenderConfig c;
    c.stereo_mode      = mode;
    c.temporal_enabled = temporal;
    return c;
}

const StereoPreset kPresets[] = {
    {"vr.multiview", 1, "1 パスで両眼を全面描画する。 差分描画なしの基準",
     make_config(StereoMode::Multiview, false), true},
    {"vr.two_pass", 1, "眼ごとに全面描画する。 multiview が無いデバイス向け",
     make_config(StereoMode::TwoPass, false), false},
    {"vr.eye_delta", 1, "左眼を描き、 右眼は再投影して穴だけ描く",
     make_config(StereoMode::EyeDelta, false), false},
    {"vr.film_delta", 1,
     "両眼差分 + 時間差分。 静的な背景が多い映像向け",
     make_config(StereoMode::EyeDelta, true), false},
};

constexpr uint32_t kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

} // namespace

/// @implements SPEC-PC-XR-STEREO-PRESETS
const StereoPreset* find_stereo_preset(std::string_view id) {
    for (const StereoPreset& p : kPresets) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

/// @implements SPEC-PC-XR-STEREO-PRESETS
uint32_t stereo_preset_count() { return kPresetCount; }

/// @implements SPEC-PC-XR-STEREO-PRESETS
const StereoPreset& stereo_preset_at(uint32_t index) { return kPresets[index]; }

} // namespace pictor::xr
