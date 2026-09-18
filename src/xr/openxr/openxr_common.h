#pragma once

// OpenXR backend の内部ヘッダ。 公開ヘッダからは取り込まない。
#ifndef NOGDI
#define NOGDI
#endif
#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstddef>
#include <cstdio>

namespace pictor::xr::detail {

/// @implements SPEC-PC-XR-OPENXR-SESSION
/// 失敗なら何が失敗したかを stderr へ出して false。 成功コード
/// (XR_SESSION_NOT_FOCUSED 等の条件付き成功を含む) は true。
inline bool xr_check(XrResult result, const char* what) {
    if (XR_SUCCEEDED(result)) return true;
    std::fprintf(stderr, "[Pictor][xr] %s failed (XrResult %d)\n", what,
                 static_cast<int>(result));
    return false;
}

/// @implements SPEC-PC-XR-OPENXR-SESSION
/// OpenXR の固定長の名前欄へ、 必ず終端して写す (長すぎる名前は切り詰める)。
template <size_t N>
inline void copy_name(char (&dst)[N], const char* src) {
    std::snprintf(dst, N, "%s", src);
}

/// @implements SPEC-PC-XR-OPENXR-SESSION
inline XrPosef identity_pose() {
    XrPosef p{};
    p.orientation.w = 1.0f;
    return p;
}

} // namespace pictor::xr::detail
