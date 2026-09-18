#include "openxr_input.h"

#include <string>
#include <vector>

namespace pictor::xr::detail {

namespace {

constexpr const char* kHandPathName[2] = {"/user/hand/left", "/user/hand/right"};

Pose to_pose(const XrPosef& p) {
    Pose out;
    out.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
    out.position    = {p.position.x, p.position.y, p.position.z};
    return out;
}

bool is_pose_valid(XrSpaceLocationFlags flags) {
    constexpr XrSpaceLocationFlags kNeeded =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    return (flags & kNeeded) == kNeeded;
}

XrPath to_path(XrInstance instance, const std::string& text) {
    XrPath path = XR_NULL_PATH;
    if (!xr_check(xrStringToPath(instance, text.c_str(), &path), text.c_str())) return XR_NULL_PATH;
    return path;
}

bool create_action(XrActionSet set, XrActionType type, const char* name,
                   const XrPath* hands, XrAction& out) {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    info.actionType = type;
    copy_name(info.actionName, name);
    copy_name(info.localizedActionName, name);
    info.countSubactionPaths = 2;
    info.subactionPaths      = hands;
    return xr_check(xrCreateAction(set, &info, &out), name);
}

struct BindingRow {
    XrAction    action;
    const char* left;   // nullptr = その手には割り当てない
    const char* right;
};

bool suggest(XrInstance instance, const char* profile, const std::vector<BindingRow>& rows) {
    std::vector<XrActionSuggestedBinding> bindings;
    for (const BindingRow& row : rows) {
        const char* suffix[2] = {row.left, row.right};
        for (int hand = 0; hand < 2; ++hand) {
            if (!suffix[hand]) continue;
            const XrPath path = to_path(instance, std::string(kHandPathName[hand]) + suffix[hand]);
            if (path == XR_NULL_PATH) return false;
            bindings.push_back({row.action, path});
        }
    }
    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile     = to_path(instance, profile);
    suggested.suggestedBindings      = bindings.data();
    suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    if (suggested.interactionProfile == XR_NULL_PATH) return false;
    return xr_check(xrSuggestInteractionProfileBindings(instance, &suggested), profile);
}

float read_float(XrSession session, XrAction action, XrPath hand) {
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action        = action;
    info.subactionPath = hand;
    XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_FAILED(xrGetActionStateFloat(session, &info, &state)) || !state.isActive) return 0.0f;
    return state.currentState;
}

bool read_bool(XrSession session, XrAction action, XrPath hand) {
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action        = action;
    info.subactionPath = hand;
    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_FAILED(xrGetActionStateBoolean(session, &info, &state)) || !state.isActive) return false;
    return state.currentState == XR_TRUE;
}

bool locate(XrSpace space, XrSpace base, XrTime time, Pose& out) {
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(space, base, time, &location))) return false;
    if (!is_pose_valid(location.locationFlags)) return false;
    out = to_pose(location.pose);
    return true;
}

} // namespace

OpenXrInput::~OpenXrInput() { shutdown(); }

bool OpenXrInput::initialize(XrInstance instance, XrSession session) {
    if (action_set_ != XR_NULL_HANDLE) return false;
    if (!create_actions_(instance) || !suggest_bindings_(instance) || !create_spaces_(session)) {
        shutdown();
        return false;
    }
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets      = &action_set_;
    if (!xr_check(xrAttachSessionActionSets(session, &attach), "xrAttachSessionActionSets")) {
        shutdown();
        return false;
    }
    return true;
}

bool OpenXrInput::create_actions_(XrInstance instance) {
    for (int hand = 0; hand < 2; ++hand) {
        hand_path_[hand] = to_path(instance, kHandPathName[hand]);
        if (hand_path_[hand] == XR_NULL_PATH) return false;
    }

    XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    copy_name(set_info.actionSetName, "pictor_input");
    copy_name(set_info.localizedActionSetName, "Pictor input");
    if (!xr_check(xrCreateActionSet(instance, &set_info, &action_set_), "xrCreateActionSet")) {
        action_set_ = XR_NULL_HANDLE;
        return false;
    }

    const XrPath* h = hand_path_;
    return create_action(action_set_, XR_ACTION_TYPE_POSE_INPUT,     "grip_pose",   h, grip_pose_) &&
           create_action(action_set_, XR_ACTION_TYPE_POSE_INPUT,     "aim_pose",    h, aim_pose_) &&
           create_action(action_set_, XR_ACTION_TYPE_FLOAT_INPUT,    "trigger",     h, trigger_) &&
           create_action(action_set_, XR_ACTION_TYPE_FLOAT_INPUT,    "squeeze",     h, squeeze_) &&
           create_action(action_set_, XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick",  h, thumbstick_) &&
           create_action(action_set_, XR_ACTION_TYPE_BOOLEAN_INPUT,  "primary",     h, primary_) &&
           create_action(action_set_, XR_ACTION_TYPE_BOOLEAN_INPUT,  "secondary",   h, secondary_) &&
           create_action(action_set_, XR_ACTION_TYPE_BOOLEAN_INPUT,  "stick_click", h, stick_click_) &&
           create_action(action_set_, XR_ACTION_TYPE_BOOLEAN_INPUT,  "menu",        h, menu_);
}

bool OpenXrInput::suggest_bindings_(XrInstance instance) {
    // Meta Quest (Touch)。 右手の system ボタンはアプリへ渡されないので割り当てない。
    const std::vector<BindingRow> touch = {
        {grip_pose_,   "/input/grip/pose",         "/input/grip/pose"},
        {aim_pose_,    "/input/aim/pose",          "/input/aim/pose"},
        {trigger_,     "/input/trigger/value",     "/input/trigger/value"},
        {squeeze_,     "/input/squeeze/value",     "/input/squeeze/value"},
        {thumbstick_,  "/input/thumbstick",        "/input/thumbstick"},
        {primary_,     "/input/x/click",           "/input/a/click"},
        {secondary_,   "/input/y/click",           "/input/b/click"},
        {stick_click_, "/input/thumbstick/click",  "/input/thumbstick/click"},
        {menu_,        "/input/menu/click",        nullptr},
    };
    // どのランタイムにもある最小のコントローラ。 Touch 以外でも頭と手の姿勢は取れる。
    const std::vector<BindingRow> simple = {
        {grip_pose_, "/input/grip/pose",    "/input/grip/pose"},
        {aim_pose_,  "/input/aim/pose",     "/input/aim/pose"},
        {trigger_,   "/input/select/click", "/input/select/click"},
        {menu_,      "/input/menu/click",   "/input/menu/click"},
    };
    return suggest(instance, "/interaction_profiles/oculus/touch_controller", touch) &&
           suggest(instance, "/interaction_profiles/khr/simple_controller", simple);
}

bool OpenXrInput::create_spaces_(XrSession session) {
    for (int hand = 0; hand < 2; ++hand) {
        XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        info.subactionPath     = hand_path_[hand];
        info.poseInActionSpace = identity_pose();
        info.action = grip_pose_;
        if (!xr_check(xrCreateActionSpace(session, &info, &grip_space_[hand]), "grip space"))
            return false;
        info.action = aim_pose_;
        if (!xr_check(xrCreateActionSpace(session, &info, &aim_space_[hand]), "aim space"))
            return false;
    }
    XrReferenceSpaceCreateInfo head{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    head.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_VIEW;
    head.poseInReferenceSpace = identity_pose();
    return xr_check(xrCreateReferenceSpace(session, &head, &head_space_), "head space");
}

void OpenXrInput::shutdown() {
    for (int hand = 0; hand < 2; ++hand) {
        if (grip_space_[hand] != XR_NULL_HANDLE) xrDestroySpace(grip_space_[hand]);
        if (aim_space_[hand]  != XR_NULL_HANDLE) xrDestroySpace(aim_space_[hand]);
        grip_space_[hand] = aim_space_[hand] = XR_NULL_HANDLE;
    }
    if (head_space_ != XR_NULL_HANDLE) xrDestroySpace(head_space_);
    head_space_ = XR_NULL_HANDLE;
    // action は action set の子で、 set の破棄でまとめて解放される。
    if (action_set_ != XR_NULL_HANDLE) xrDestroyActionSet(action_set_);
    action_set_ = XR_NULL_HANDLE;
    grip_pose_ = aim_pose_ = trigger_ = squeeze_ = thumbstick_ = XR_NULL_HANDLE;
    primary_ = secondary_ = stick_click_ = menu_ = XR_NULL_HANDLE;
}

bool OpenXrInput::sample(XrSession session, XrSpace base_space, XrTime time,
                         InputSnapshot& out) {
    if (action_set_ == XR_NULL_HANDLE) return false;
    out = InputSnapshot{};
    out.is_head_tracked = locate(head_space_, base_space, time, out.head_pose);

    XrActiveActionSet active{action_set_, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets      = &active;
    const XrResult synced = xrSyncActions(session, &sync);
    if (!xr_check(synced, "xrSyncActions")) return false;
    // フォーカスが無い間は入力が来ない。 失敗ではないので、 手は未追跡のまま返す。
    if (synced == XR_SESSION_NOT_FOCUSED) return true;

    for (int hand = 0; hand < 2; ++hand) {
        ControllerState& c = out.hands[hand];
        const XrPath path = hand_path_[hand];
        c.is_tracked = locate(grip_space_[hand], base_space, time, c.grip_pose);
        if (c.is_tracked) locate(aim_space_[hand], base_space, time, c.aim_pose);

        c.trigger = read_float(session, trigger_, path);
        c.squeeze = read_float(session, squeeze_, path);

        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action        = thumbstick_;
        info.subactionPath = path;
        XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &info, &stick)) && stick.isActive) {
            c.thumbstick_x = stick.currentState.x;
            c.thumbstick_y = stick.currentState.y;
        }

        if (read_bool(session, primary_, path))     c.buttons |= kButtonPrimary;
        if (read_bool(session, secondary_, path))   c.buttons |= kButtonSecondary;
        if (read_bool(session, stick_click_, path)) c.buttons |= kButtonThumbstick;
        if (read_bool(session, menu_, path))        c.buttons |= kButtonMenu;
    }
    return true;
}

} // namespace pictor::xr::detail
