// DeltaRenderPlanner: いつ全面描画し、 いつ再投影で済ませるかの契約。

#include "pictor/xr/delta_render_planner.h"
#include "pictor/xr/stereo_camera.h"
#include "pictor/xr/stereo_presets.h"
#include "test_common.h"

using namespace pictor;
using namespace pictor::xr;

namespace {

constexpr uint64_t kEyePixels = 1000;

StereoCamera make_stereo(float head_x = 0.0f) {
    ViewState v;
    v.is_valid = true;
    v.eyes[0].pose.position = {head_x - 0.032f, 0.0f, 0.0f};
    v.eyes[1].pose.position = {head_x + 0.032f, 0.0f, 0.0f};
    v.eyes[0].fov = {-0.94f, 0.70f, 0.84f, -0.96f};
    v.eyes[1].fov = {-0.70f, 0.94f, 0.84f, -0.96f};
    StereoCamera cam;
    cam.update(v);
    return cam;
}

FramePlan plan_at(DeltaRenderPlanner& planner, float head_x = 0.0f) {
    const StereoCamera cam = make_stereo(head_x);
    return planner.plan(cam.eye(Eye::Left), cam.eye(Eye::Right));
}

DeltaRenderConfig preset(const char* id) {
    const StereoPreset* p = find_stereo_preset(id);
    PT_ASSERT(p != nullptr, "preset exists");
    return p ? p->config : DeltaRenderConfig{};
}

void test_presets() {
    PT_ASSERT(find_stereo_preset("vr.no_such_preset") == nullptr, "unknown id is not replaced");
    PT_ASSERT(stereo_preset_count() >= 4, "the four documented presets are registered");
    const StereoPreset* multiview = find_stereo_preset("vr.multiview");
    PT_ASSERT(multiview && multiview->requires_multiview, "multiview preset declares its need");
    const StereoPreset* film = find_stereo_preset("vr.film_delta");
    PT_ASSERT(film && film->config.temporal_enabled &&
                  film->config.stereo_mode == StereoMode::EyeDelta,
              "film preset enables both delta kinds");
}

void test_multiview_never_reprojects() {
    DeltaRenderPlanner planner(preset("vr.multiview"));
    for (int i = 0; i < 3; ++i) {
        const FramePlan p = plan_at(planner);
        PT_ASSERT(p.stereo_mode == StereoMode::Multiview, "mode is carried");
        PT_ASSERT(p.eyes[0].source == StaticLayerSource::FullRedraw &&
                      p.eyes[1].source == StaticLayerSource::FullRedraw,
                  "multiview draws both eyes in full");
        PT_ASSERT(!p.eyes[0].capture_keyframe && !p.eyes[1].capture_keyframe,
                  "multiview captures nothing");
    }
}

void test_eye_delta_without_temporal() {
    DeltaRenderPlanner planner(preset("vr.eye_delta"));
    for (int i = 0; i < 3; ++i) {
        const FramePlan p = plan_at(planner);
        PT_ASSERT(p.keyframe_reason == KeyframeReason::TemporalDisabled, "every frame restarts");
        PT_ASSERT(p.eyes[0].source == StaticLayerSource::FullRedraw, "left eye is drawn in full");
        PT_ASSERT(p.eyes[0].capture_keyframe, "left eye is captured as the right eye's source");
        PT_ASSERT(p.eyes[1].source == StaticLayerSource::OtherEyeKeyframe,
                  "right eye is reprojected from the left");
        PT_ASSERT(!p.eyes[1].capture_keyframe, "right eye is not kept without temporal delta");
    }
}

void test_temporal_reuses_keyframe_until_limits() {
    DeltaRenderConfig cfg = preset("vr.film_delta");
    cfg.max_keyframe_age = 5;
    DeltaRenderPlanner planner(cfg);

    FramePlan p = plan_at(planner);
    PT_ASSERT(p.keyframe_reason == KeyframeReason::FirstFrame, "first frame is a keyframe");
    PT_ASSERT(p.eyes[0].capture_keyframe && p.eyes[1].capture_keyframe, "both eyes are captured");

    p = plan_at(planner, 0.01f);
    PT_ASSERT(p.keyframe_reason == KeyframeReason::None, "a small move reuses the keyframe");
    PT_ASSERT(p.eyes[0].source == StaticLayerSource::OwnKeyframe &&
                  p.eyes[1].source == StaticLayerSource::OwnKeyframe,
              "both eyes reproject their own keyframe");
    PT_ASSERT(p.source_camera[0].position.x < p.source_camera[1].position.x,
              "source cameras are the keyframe's eyes, not the current ones");

    p = plan_at(planner, 0.5f);
    PT_ASSERT(p.keyframe_reason == KeyframeReason::MovedTooFar, "a large move retakes it");

    planner.notify_static_scene_changed();
    p = plan_at(planner, 0.5f);
    PT_ASSERT(p.keyframe_reason == KeyframeReason::StaticSceneChanged,
              "a static scene change retakes it");

    KeyframeReason last = KeyframeReason::None;
    for (int i = 0; i < 6; ++i) last = plan_at(planner, 0.5f).keyframe_reason;
    PT_ASSERT(last == KeyframeReason::TooOld, "an old keyframe is retaken");
}

void test_hole_feedback() {
    DeltaRenderPlanner planner(preset("vr.film_delta"));
    plan_at(planner);                      // キーフレーム (右眼は左眼から再投影)
    plan_at(planner, 0.01f);               // 使い回し
    planner.report_hole_pixels(Eye::Left, 600, kEyePixels);
    PT_ASSERT(planner.last_hole_ratio(Eye::Left) > 0.5f, "ratio is recorded");
    FramePlan p = plan_at(planner, 0.01f);
    PT_ASSERT(p.keyframe_reason == KeyframeReason::TooManyHoles,
              "too many holes while reusing retakes the keyframe");

    // 取り直しの直後に古い比率でもう一度取り直さない。
    p = plan_at(planner, 0.01f);
    PT_ASSERT(p.keyframe_reason == KeyframeReason::None, "stale ratio does not retrigger");

    planner.report_hole_pixels(Eye::Left, 10, 0);
    PT_ASSERT(planner.last_hole_ratio(Eye::Left) < 0.01f, "zero pixel count is ignored");
}

void test_eye_delta_cooldown() {
    DeltaRenderConfig cfg = preset("vr.eye_delta");
    cfg.eye_delta_cooldown = 3;
    DeltaRenderPlanner planner(cfg);

    plan_at(planner);
    planner.report_hole_pixels(Eye::Right, 900, kEyePixels);  // 被写体が近く視差が大きい

    FramePlan p = plan_at(planner);
    PT_ASSERT(p.eyes[1].source == StaticLayerSource::FullRedraw,
              "right eye falls back to a full redraw while cooling down");
    p = plan_at(planner);
    PT_ASSERT(p.eyes[1].source == StaticLayerSource::FullRedraw, "cooldown lasts its frames");
    p = plan_at(planner);
    PT_ASSERT(p.eyes[1].source == StaticLayerSource::OtherEyeKeyframe,
              "eye delta resumes after the cooldown");
}

void test_reset_drops_keyframe() {
    DeltaRenderPlanner planner(preset("vr.film_delta"));
    plan_at(planner);
    planner.reset();
    PT_ASSERT(plan_at(planner).keyframe_reason == KeyframeReason::FirstFrame,
              "reset forces a new keyframe");
}

} // namespace

int main() {
    test_presets();
    test_multiview_never_reprojects();
    test_eye_delta_without_temporal();
    test_temporal_reuses_keyframe_until_limits();
    test_hole_feedback();
    test_eye_delta_cooldown();
    test_reset_drops_keyframe();
    return pictor_test::report("unit_xr_delta_planner_test");
}
