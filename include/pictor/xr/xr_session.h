#pragma once

#include "pictor/xr/xr_types.h"

namespace pictor::xr {

/// XR ランタイムとのセッション境界。
///
/// 1 フレームの呼び出し順 (実装はこの順を前提にしてよい):
///   poll_events → begin_frame → locate_views / sample_input
///   → (描画) → end_frame
///
/// 失敗は戻り値で返す。 セッション喪失後は `state() == SessionState::Lost` になり、
/// host が作り直す。 実装を差し替えても (OpenXR / テスト用の記録再生) この契約は変えない。
class IXrSession {
public:
    virtual ~IXrSession() = default;

    virtual SessionState state() const = 0;

    /// ランタイムのイベントを処理して状態を進める。
    /// false はアプリ終了要求 (`SessionState::Exiting`)。
    virtual bool poll_events() = 0;

    /// フレームを開始し、 表示予定時刻を受け取る。
    /// セッションが走っていない間は false (描画も end_frame も不要)。
    virtual bool begin_frame(FrameTiming& out_timing) = 0;

    /// `timing` の表示時刻における両眼の姿勢と視野。
    virtual bool locate_views(const FrameTiming& timing, ViewState& out_views) = 0;

    /// `timing` の表示時刻における頭とコントローラの状態。
    virtual bool sample_input(const FrameTiming& timing, InputSnapshot& out_input) = 0;

    /// フレームを提出する。 `has_layer == false` は何も描かなかったフレーム。
    /// begin_frame が true を返したフレームでは必ず呼ぶ。
    virtual bool end_frame(const FrameTiming& timing, const ViewState& views,
                           bool has_layer) = 0;

    /// ランタイムが推奨する片眼あたりの描画解像度。
    virtual EyeExtent recommended_eye_extent() const = 0;
};

} // namespace pictor::xr
