#include "pictor/tap/frame_tap_vocabulary.h"

namespace pictor {

const char* frame_tap_pass_kind_name(FrameTapPassKind kind) {
    switch (kind) {
        case FrameTapPassKind::SCENE:         return "scene";
        case FrameTapPassKind::UI:            return "ui";
        case FrameTapPassKind::SHADOW:        return "shadow";
        case FrameTapPassKind::REFLECTION:    return "reflection";
        case FrameTapPassKind::DEPTH_PREPASS: return "depth-prepass";
        case FrameTapPassKind::POSTPROCESS:   return "postprocess";
        case FrameTapPassKind::OTHER:         return "other";
    }
    return "other";
}

const char* frame_tap_visibility_name(FrameTapVisibility visibility) {
    switch (visibility) {
        case FrameTapVisibility::OCCLUSION_PASSED: return "occlusion-passed";
        case FrameTapVisibility::OCCLUSION_FAILED: return "occlusion-failed";
        case FrameTapVisibility::FRUSTUM_ONLY:     return "frustum-only";
        case FrameTapVisibility::UNKNOWN:          return "unknown";
    }
    return "unknown";
}

const char* frame_tap_identity_name(FrameTapIdentity identity) {
    switch (identity) {
        case FrameTapIdentity::ASSET_NAME:   return "asset-name";
        case FrameTapIdentity::CONTENT_HASH: return "content-hash";
        case FrameTapIdentity::COUNT_HASH:   return "count-hash";
    }
    return "count-hash";
}

const char* frame_tap_ui_role_name(FrameTapUiRole role) {
    switch (role) {
        case FrameTapUiRole::BAR:   return "bar";
        case FrameTapUiRole::GLYPH: return "glyph";
        case FrameTapUiRole::NONE:  break;
    }
    return "";
}

const char* frame_tap_end_reason_name(FrameTapEndReason reason) {
    switch (reason) {
        case FrameTapEndReason::SHUTDOWN: return "shutdown";
        case FrameTapEndReason::FAILED:   return "error";
    }
    return "error";
}

const char* frame_tap_drop_reason_name(FrameTapDropReason reason) {
    switch (reason) {
        case FrameTapDropReason::BACKPRESSURE: return "backpressure";
        case FrameTapDropReason::BUFFER_FULL:  return "buffer-full";
        case FrameTapDropReason::OTHER:        return "other";
    }
    return "other";
}

bool is_valid_frame_tap_observer_id(std::string_view id) {
    if (id.empty()) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (alnum) continue;
        if (i != 0 && (c == '_' || c == '.' || c == '-')) continue;
        return false;
    }
    return true;
}

} // namespace pictor
