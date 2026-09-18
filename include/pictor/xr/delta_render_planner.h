#pragma once

#include "pictor/core/camera.h"
#include "pictor/xr/xr_types.h"
#include <cstdint>

namespace pictor::xr {

/// 両眼をどう描くか。
enum class StereoMode : uint8_t {
    Multiview,  // 1 パスで両眼を全面描画 (VK_KHR_multiview)。 差分描画は使わない
    TwoPass,    // 眼ごとに全面描画
    EyeDelta,   // 左眼を描き、 右眼は左眼の画を再投影して穴だけ描く
};

/// 眼の静的層を何から作るか。 動的層はどの場合も毎フレーム上から描く。
enum class StaticLayerSource : uint8_t {
    FullRedraw,       // 静的層を全面描画する
    OwnKeyframe,      // 自分の眼のキーフレームを再投影し、 穴だけ描く
    OtherEyeKeyframe, // もう一方の眼のキーフレームを再投影し、 穴だけ描く
};

/// キーフレームを取り直した理由 (計測とテスト用)。
enum class KeyframeReason : uint8_t {
    None,               // キーフレームを使い回した
    TemporalDisabled,   // 時間差分が無効なので毎フレーム取り直す
    FirstFrame,
    StaticSceneChanged, // host が静的層の変更を通知した
    MovedTooFar,
    TurnedTooFar,
    TooOld,
    TooManyHoles,       // 穴埋めで描いた画素が多すぎた
};

struct DeltaRenderConfig {
    StereoMode stereo_mode      = StereoMode::Multiview;
    bool       temporal_enabled = false;

    // キーフレームを使い回す上限。 超えたら取り直す (= その眼を全面描画する)。
    float    max_translation_m  = 0.10f;
    float    max_rotation_rad   = 0.35f;   // 約 20 度
    uint32_t max_keyframe_age   = 90;      // フレーム数
    /// 穴埋めで描いた画素 / 眼の全画素 がこれを超えたら、 再投影より全面描画が速い。
    float    max_hole_ratio     = 0.25f;
    /// 両眼差分で穴が多すぎたとき、 このフレーム数だけ右眼を全面描画に戻す。
    uint32_t eye_delta_cooldown = 120;
};

struct EyePlan {
    StaticLayerSource source = StaticLayerSource::FullRedraw;
    /// true のとき、 この眼の静的層をキーフレームとして取り直す。
    bool capture_keyframe = false;
};

struct FramePlan {
    StereoMode     stereo_mode = StereoMode::Multiview;
    EyePlan        eyes[kEyeCount];
    KeyframeReason keyframe_reason = KeyframeReason::None;
    /// eyes[i].source が再投影のとき、 再投影元のカメラ (キーフレームを撮った視点)。
    Camera         source_camera[kEyeCount];
};

/// 1 フレームごとの差分描画の計画を決める。 GPU には触れない。
///
/// 使い方: フレームの頭で plan() を呼び、 計画どおりに描き、 穴埋めで描いた
/// 画素数が分かったら report_hole_pixels() で戻す (次のフレーム以降の判断に使う)。
class DeltaRenderPlanner {
public:
    explicit DeltaRenderPlanner(const DeltaRenderConfig& config = {});

    /// 設定を差し替える。 方式が変わるとキーフレームの前提が崩れるので捨てる。
    void set_config(const DeltaRenderConfig& config);
    const DeltaRenderConfig& config() const { return config_; }

    /// 静的層の中身が変わったことを知らせる。 次の plan() でキーフレームを取り直す。
    void notify_static_scene_changed() { is_static_scene_dirty_ = true; }

    /// キーフレームを捨てる (描画先の作り直し・セッション喪失の後に呼ぶ)。
    void reset();

    FramePlan plan(const Camera& left, const Camera& right);

    /// 直前の plan() のフレームで、 穴埋めとして実際に描かれた画素数。
    /// eye_pixel_count が 0 の報告は不正入力として無視する。
    void report_hole_pixels(Eye eye, uint64_t hole_pixels, uint64_t eye_pixel_count);

    /// 直近に報告された穴の比率 (報告が無ければ 0)。
    float last_hole_ratio(Eye eye) const {
        return last_hole_ratio_[static_cast<uint32_t>(eye)];
    }

private:
    KeyframeReason keyframe_reason_(const Camera& left, const Camera& right) const;

    DeltaRenderConfig config_;
    Camera   keyframe_camera_[kEyeCount];
    bool     has_keyframe_            = false;
    bool     is_static_scene_dirty_   = false;
    uint32_t keyframe_age_            = 0;
    uint32_t eye_delta_cooldown_left_ = 0;
    float    last_hole_ratio_[kEyeCount] = {0.0f, 0.0f};
    /// 直前のフレームで右眼を左眼から再投影したか。 穴の報告を、 時間差分と
    /// 両眼差分のどちらの判断に効かせるかの区別に使う。
    bool     was_right_eye_from_left_ = false;
};

} // namespace pictor::xr
