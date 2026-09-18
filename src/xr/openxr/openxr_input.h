#pragma once

#include "openxr_common.h"
#include "pictor/xr/xr_types.h"

namespace pictor::xr::detail {

/// コントローラ入力の取得。 action の定義・割り当ての提案・毎フレームの読み出しを持つ。
/// 値を運ぶだけで、 ボタンの意味付けはしない (host の責務)。
///
/// XrActionSet / XrAction / XrSpace はセッションの子なので、 セッションを破棄する
/// 前に shutdown() を呼ぶこと。
class OpenXrInput {
public:
    OpenXrInput() = default;
    ~OpenXrInput();

    OpenXrInput(const OpenXrInput&)            = delete;
    OpenXrInput& operator=(const OpenXrInput&) = delete;

    bool initialize(XrInstance instance, XrSession session);
    void shutdown();

    /// `base_space` に対する頭と両手の状態を `time` の時点で読む。
    /// 入力フォーカスが無い間は、 手を「未追跡」にして true を返す。
    bool sample(XrSession session, XrSpace base_space, XrTime time, InputSnapshot& out);

private:
    bool create_actions_(XrInstance instance);
    bool suggest_bindings_(XrInstance instance);
    bool create_spaces_(XrSession session);

    XrActionSet action_set_ = XR_NULL_HANDLE;
    XrAction grip_pose_   = XR_NULL_HANDLE;
    XrAction aim_pose_    = XR_NULL_HANDLE;
    XrAction trigger_     = XR_NULL_HANDLE;
    XrAction squeeze_     = XR_NULL_HANDLE;
    XrAction thumbstick_  = XR_NULL_HANDLE;
    XrAction primary_     = XR_NULL_HANDLE;
    XrAction secondary_   = XR_NULL_HANDLE;
    XrAction stick_click_ = XR_NULL_HANDLE;
    XrAction menu_        = XR_NULL_HANDLE;

    XrPath  hand_path_[2]  = {XR_NULL_PATH, XR_NULL_PATH};
    XrSpace grip_space_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrSpace aim_space_[2]  = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrSpace head_space_    = XR_NULL_HANDLE;
};

} // namespace pictor::xr::detail
