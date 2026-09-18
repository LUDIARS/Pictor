#include "openxr_session.h"

namespace pictor::xr::detail {

namespace {

constexpr XrViewConfigurationType kViewConfig = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

EyeView to_eye_view(const XrView& v) {
    EyeView out;
    out.pose.orientation = {v.pose.orientation.x, v.pose.orientation.y,
                            v.pose.orientation.z, v.pose.orientation.w};
    out.pose.position    = {v.pose.position.x, v.pose.position.y, v.pose.position.z};
    out.fov = {v.fov.angleLeft, v.fov.angleRight, v.fov.angleUp, v.fov.angleDown};
    return out;
}

XrPosef to_xr_pose(const Pose& p) {
    XrPosef out{};
    out.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
    out.position    = {p.position.x, p.position.y, p.position.z};
    return out;
}

} // namespace

OpenXrSession::~OpenXrSession() { shutdown(); }

bool OpenXrSession::initialize(XrInstance instance, XrSystemId system,
                               const XrGraphicsBindingVulkanKHR& binding,
                               EyeExtent recommended) {
    if (session_ != XR_NULL_HANDLE) return false;
    instance_    = instance;
    recommended_ = recommended;

    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next     = &binding;
    info.systemId = system;
    if (!xr_check(xrCreateSession(instance, &info, &session_), "xrCreateSession")) {
        session_ = XR_NULL_HANDLE;
        return false;
    }

    // LOCAL = 起動時の頭の位置を原点にした座った姿勢向けの空間。 映像鑑賞に合う。
    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space.poseInReferenceSpace = identity_pose();
    if (!xr_check(xrCreateReferenceSpace(session_, &space, &base_space_), "base space") ||
        !input_.initialize(instance, session_)) {
        shutdown();
        return false;
    }
    return true;
}

void OpenXrSession::shutdown() {
    // 子 (入力の空間・基準空間) を先に、 セッションを最後に破棄する。
    input_.shutdown();
    if (base_space_ != XR_NULL_HANDLE) xrDestroySpace(base_space_);
    base_space_ = XR_NULL_HANDLE;
    if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
    session_    = XR_NULL_HANDLE;
    is_running_ = false;
    state_      = SessionState::Idle;
}

void OpenXrSession::on_state_changed_(XrSessionState next) {
    switch (next) {
    case XR_SESSION_STATE_READY: {
        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
        begin.primaryViewConfigurationType = kViewConfig;
        if (xr_check(xrBeginSession(session_, &begin), "xrBeginSession")) {
            is_running_ = true;
            state_      = SessionState::Ready;
        } else {
            state_ = SessionState::Lost;
        }
        break;
    }
    case XR_SESSION_STATE_SYNCHRONIZED: state_ = SessionState::Ready;   break;
    case XR_SESSION_STATE_VISIBLE:      state_ = SessionState::Visible; break;
    case XR_SESSION_STATE_FOCUSED:      state_ = SessionState::Focused; break;
    case XR_SESSION_STATE_STOPPING:
        if (is_running_) xr_check(xrEndSession(session_), "xrEndSession");
        is_running_ = false;
        state_      = SessionState::Stopping;
        break;
    case XR_SESSION_STATE_LOSS_PENDING: state_ = SessionState::Lost;    break;
    case XR_SESSION_STATE_EXITING:      state_ = SessionState::Exiting; break;
    default:                            state_ = SessionState::Idle;    break;
    }
}

bool OpenXrSession::poll_events() {
    if (session_ == XR_NULL_HANDLE) return false;

    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            if (changed->session == session_) on_state_changed_(changed->state);
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            state_ = SessionState::Lost;
        }
        event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
    return state_ != SessionState::Exiting;
}

bool OpenXrSession::begin_frame(FrameTiming& out_timing) {
    if (!is_running_) return false;

    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame{XR_TYPE_FRAME_STATE};
    if (!xr_check(xrWaitFrame(session_, &wait, &frame), "xrWaitFrame")) return false;

    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
    if (!xr_check(xrBeginFrame(session_, &begin), "xrBeginFrame")) return false;

    out_timing.predicted_display_time_ns   = frame.predictedDisplayTime;
    out_timing.predicted_display_period_ns = frame.predictedDisplayPeriod;
    out_timing.should_render               = frame.shouldRender == XR_TRUE;
    return true;
}

bool OpenXrSession::locate_views(const FrameTiming& timing, ViewState& out_views) {
    out_views.is_valid = false;
    if (!is_running_) return false;

    XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
    info.viewConfigurationType = kViewConfig;
    info.displayTime           = timing.predicted_display_time_ns;
    info.space                 = base_space_;

    XrViewState view_state{XR_TYPE_VIEW_STATE};
    XrView views[kEyeCount] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t count = 0;
    if (!xr_check(xrLocateViews(session_, &info, &view_state, kEyeCount, &count, views),
                  "xrLocateViews")) return false;
    if (count != kEyeCount) return false;

    constexpr XrViewStateFlags kNeeded =
        XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    for (uint32_t i = 0; i < kEyeCount; ++i) out_views.eyes[i] = to_eye_view(views[i]);
    out_views.is_valid = (view_state.viewStateFlags & kNeeded) == kNeeded;
    return true;
}

bool OpenXrSession::sample_input(const FrameTiming& timing, InputSnapshot& out_input) {
    if (!is_running_) return false;
    return input_.sample(session_, base_space_, timing.predicted_display_time_ns, out_input);
}

bool OpenXrSession::end_frame(const FrameTiming& timing, const ViewState& views,
                              bool has_layer) {
    if (!is_running_) return false;

    XrCompositionLayerProjectionView projection_views[kEyeCount]{};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[1] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};

    // 描いていない・姿勢が無効・描画先が無いフレームは、 層なしで提出する
    // (古い画を正しくない姿勢で出すより、 ランタイムの既定表示に任せる)。
    const bool submit_layer = has_layer && timing.should_render && views.is_valid &&
                              target_.swapchain != XR_NULL_HANDLE;
    if (submit_layer) {
        for (uint32_t i = 0; i < kEyeCount; ++i) {
            XrCompositionLayerProjectionView& v = projection_views[i];
            v.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            v.pose = to_xr_pose(views.eyes[i].pose);
            v.fov  = {views.eyes[i].fov.angle_left, views.eyes[i].fov.angle_right,
                      views.eyes[i].fov.angle_up, views.eyes[i].fov.angle_down};
            v.subImage.swapchain        = target_.swapchain;
            v.subImage.imageArrayIndex  = i;
            v.subImage.imageRect.extent = {static_cast<int32_t>(target_.extent.width),
                                           static_cast<int32_t>(target_.extent.height)};
        }
        layer.space     = base_space_;
        layer.viewCount = kEyeCount;
        layer.views     = projection_views;
    }

    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime          = timing.predicted_display_time_ns;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount           = submit_layer ? 1u : 0u;
    end.layers               = submit_layer ? layers : nullptr;
    return xr_check(xrEndFrame(session_, &end), "xrEndFrame");
}

} // namespace pictor::xr::detail
