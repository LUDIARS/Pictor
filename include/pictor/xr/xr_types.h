#pragma once

#include "pictor/core/types.h"
#include <cstdint>

/// XR (HMD + コントローラ) の backend 非依存な値型。
///
/// OpenXR の C 型 (`XrPosef` / `XrViewState` 等) と名前が衝突しないよう
/// `pictor::xr` 名前空間に閉じ込める。 OpenXR のヘッダはここへ持ち込まない。
namespace pictor::xr {

constexpr uint32_t kEyeCount = 2;

enum class Eye : uint8_t { Left = 0, Right = 1 };
enum class Hand : uint8_t { Left = 0, Right = 1 };

/// トラッキング空間での姿勢。 右手系、 -z が前方、 +y が上、 単位はメートル。
struct Pose {
    float4 orientation{0.0f, 0.0f, 0.0f, 1.0f};  // 四元数 (x, y, z, w)
    float3 position;
};

/// 視線軸からの視野角 (ラジアン)。 左と下は負、 右と上は正。
struct Fov {
    float angle_left  = 0.0f;
    float angle_right = 0.0f;
    float angle_up    = 0.0f;
    float angle_down  = 0.0f;
};

struct EyeView {
    Pose pose;
    Fov  fov;
};

/// 1 フレーム分の両眼の姿勢。 `is_valid == false` の間はカメラへ反映しない。
struct ViewState {
    EyeView eyes[kEyeCount];
    bool    is_valid = false;
};

/// ランタイムが予告した表示時刻。 姿勢の取得と提出は同じ値で行う。
struct FrameTiming {
    int64_t predicted_display_time_ns   = 0;
    int64_t predicted_display_period_ns = 0;
    /// false のフレームは描画を省いてよい (HMD を外している等)。 提出は必要。
    bool    should_render = false;
};

/// コントローラの生の状態。 ボタンの意味付けは host の責務 (PC-RULE-001) で、
/// Pictor は値を運ぶだけ。
enum ControllerButton : uint32_t {
    kButtonPrimary    = 1u << 0,  // 右手 A / 左手 X
    kButtonSecondary  = 1u << 1,  // 右手 B / 左手 Y
    kButtonThumbstick = 1u << 2,  // スティック押し込み
    kButtonMenu       = 1u << 3,
};

struct ControllerState {
    Pose     grip_pose;   // 握りの中心
    Pose     aim_pose;    // 指し示す向き
    bool     is_tracked = false;
    float    trigger      = 0.0f;  // [0,1]
    float    squeeze      = 0.0f;  // [0,1]
    float    thumbstick_x = 0.0f;  // [-1,1]
    float    thumbstick_y = 0.0f;  // [-1,1]
    uint32_t buttons      = 0;     // ControllerButton のビット和
};

struct InputSnapshot {
    Pose            head_pose;
    bool            is_head_tracked = false;
    ControllerState hands[2];
};

enum class SessionState : uint8_t {
    Idle,        // セッション未開始 (ランタイムの準備待ち)
    Ready,       // 開始済み、 まだ表示されていない
    Visible,     // 表示中、 入力フォーカス無し
    Focused,     // 表示中、 入力フォーカスあり
    Stopping,    // ランタイムが終了を要求
    Lost,        // セッション喪失。 再初期化が必要
    Exiting,     // アプリ終了要求
};

struct EyeExtent {
    uint32_t width  = 0;
    uint32_t height = 0;
};

} // namespace pictor::xr
