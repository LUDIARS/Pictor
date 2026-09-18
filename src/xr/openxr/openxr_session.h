#pragma once

#include "openxr_common.h"
#include "openxr_input.h"
#include "pictor/xr/xr_session.h"

namespace pictor::xr::detail {

/// 提出する描画先の情報。 セッションは swapchain を所有しない。
struct ProjectionTarget {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    EyeExtent   extent;
};

/// OpenXR のセッション状態とフレームループ。
/// XrSession と基準空間 (LOCAL) を所有する。 XrInstance は借用。
class OpenXrSession final : public IXrSession {
public:
    OpenXrSession() = default;
    ~OpenXrSession() override;

    OpenXrSession(const OpenXrSession&)            = delete;
    OpenXrSession& operator=(const OpenXrSession&) = delete;

    bool initialize(XrInstance instance, XrSystemId system,
                    const XrGraphicsBindingVulkanKHR& binding, EyeExtent recommended);
    void shutdown();

    XrSession handle() const { return session_; }
    void set_projection_target(const ProjectionTarget& target) { target_ = target; }

    SessionState state() const override { return state_; }
    bool poll_events() override;
    bool begin_frame(FrameTiming& out_timing) override;
    bool locate_views(const FrameTiming& timing, ViewState& out_views) override;
    bool sample_input(const FrameTiming& timing, InputSnapshot& out_input) override;
    bool end_frame(const FrameTiming& timing, const ViewState& views, bool has_layer) override;
    EyeExtent recommended_eye_extent() const override { return recommended_; }

private:
    void on_state_changed_(XrSessionState next);

    XrInstance instance_   = XR_NULL_HANDLE;
    XrSession  session_    = XR_NULL_HANDLE;
    XrSpace    base_space_ = XR_NULL_HANDLE;
    OpenXrInput input_;

    ProjectionTarget target_;
    EyeExtent        recommended_;
    SessionState     state_      = SessionState::Idle;
    bool             is_running_ = false;  // xrBeginSession 済みで xrEndSession 前
};

} // namespace pictor::xr::detail
