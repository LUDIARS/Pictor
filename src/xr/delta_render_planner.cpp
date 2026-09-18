#include "pictor/xr/delta_render_planner.h"

#include "pictor/xr/reprojection.h"

#include <algorithm>

namespace pictor::xr {

namespace {
constexpr uint32_t kLeft  = static_cast<uint32_t>(Eye::Left);
constexpr uint32_t kRight = static_cast<uint32_t>(Eye::Right);
} // namespace

DeltaRenderPlanner::DeltaRenderPlanner(const DeltaRenderConfig& config)
    : config_(config) {}

void DeltaRenderPlanner::set_config(const DeltaRenderConfig& config) {
    config_ = config;
    reset();
}

void DeltaRenderPlanner::reset() {
    has_keyframe_            = false;
    is_static_scene_dirty_   = false;
    keyframe_age_            = 0;
    eye_delta_cooldown_left_ = 0;
    was_right_eye_from_left_ = false;
    last_hole_ratio_[kLeft] = last_hole_ratio_[kRight] = 0.0f;
}

KeyframeReason DeltaRenderPlanner::keyframe_reason_(const Camera& left,
                                                    const Camera& right) const {
    if (!config_.temporal_enabled) return KeyframeReason::TemporalDisabled;
    if (!has_keyframe_)            return KeyframeReason::FirstFrame;
    if (is_static_scene_dirty_)    return KeyframeReason::StaticSceneChanged;
    if (keyframe_age_ >= config_.max_keyframe_age) return KeyframeReason::TooOld;

    const Camera* now[kEyeCount] = {&left, &right};
    for (uint32_t i = 0; i < kEyeCount; ++i) {
        const ViewDelta d = measure_view_delta(keyframe_camera_[i], *now[i]);
        if (d.translation_m > config_.max_translation_m) return KeyframeReason::MovedTooFar;
        if (d.rotation_rad  > config_.max_rotation_rad)  return KeyframeReason::TurnedTooFar;
    }
    // 直前のフレームがキーフレームの使い回しで、 それでも穴が多かった場合だけ効かせる。
    // 両眼差分の穴は視差によるもので、 キーフレームを取り直しても減らない。
    if (!was_right_eye_from_left_ &&
        std::max(last_hole_ratio_[kLeft], last_hole_ratio_[kRight]) > config_.max_hole_ratio) {
        return KeyframeReason::TooManyHoles;
    }
    return KeyframeReason::None;
}

FramePlan DeltaRenderPlanner::plan(const Camera& left, const Camera& right) {
    FramePlan out;
    out.stereo_mode = config_.stereo_mode;

    // マルチビューは 1 パスで両眼を描き切るので、 再投影を挟む余地が無い。
    if (config_.stereo_mode == StereoMode::Multiview) {
        out.keyframe_reason = KeyframeReason::TemporalDisabled;
        was_right_eye_from_left_ = false;
        return out;
    }

    if (eye_delta_cooldown_left_ > 0) --eye_delta_cooldown_left_;
    const bool is_eye_delta_active =
        config_.stereo_mode == StereoMode::EyeDelta && eye_delta_cooldown_left_ == 0;

    out.keyframe_reason = keyframe_reason_(left, right);

    if (out.keyframe_reason != KeyframeReason::None) {
        out.eyes[kLeft].source = StaticLayerSource::FullRedraw;
        out.eyes[kRight].source = is_eye_delta_active
            ? StaticLayerSource::OtherEyeKeyframe
            : StaticLayerSource::FullRedraw;
        // 時間差分が無効でも、 両眼差分の再投影元として左眼のキーフレームは要る。
        out.eyes[kLeft].capture_keyframe  = config_.temporal_enabled || is_eye_delta_active;
        out.eyes[kRight].capture_keyframe = config_.temporal_enabled;
        out.source_camera[kRight] = left;

        keyframe_camera_[kLeft]  = left;
        keyframe_camera_[kRight] = right;
        has_keyframe_          = config_.temporal_enabled;
        is_static_scene_dirty_ = false;
        keyframe_age_          = 0;
        // 取り直した直後に古い比率で再び取り直さないようにする。
        last_hole_ratio_[kLeft] = last_hole_ratio_[kRight] = 0.0f;
    } else {
        for (uint32_t i = 0; i < kEyeCount; ++i) {
            out.eyes[i].source   = StaticLayerSource::OwnKeyframe;
            out.source_camera[i] = keyframe_camera_[i];
        }
        ++keyframe_age_;
    }

    was_right_eye_from_left_ =
        out.eyes[kRight].source == StaticLayerSource::OtherEyeKeyframe;
    return out;
}

void DeltaRenderPlanner::report_hole_pixels(Eye eye, uint64_t hole_pixels,
                                            uint64_t eye_pixel_count) {
    if (eye_pixel_count == 0) return;
    const uint32_t i = static_cast<uint32_t>(eye);
    last_hole_ratio_[i] = static_cast<float>(
        static_cast<double>(hole_pixels) / static_cast<double>(eye_pixel_count));

    // 左眼からの再投影で右眼の穴が多すぎるなら、 しばらく右眼は全面描画に戻す。
    // 被写体が近い場面では視差が大きく、 再投影の方が遅くなるため。
    if (eye == Eye::Right && was_right_eye_from_left_ &&
        last_hole_ratio_[i] > config_.max_hole_ratio) {
        eye_delta_cooldown_left_ = config_.eye_delta_cooldown;
    }
}

} // namespace pictor::xr
