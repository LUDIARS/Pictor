#pragma once

#include <vulkan/vulkan.h>

#include <cstdio>

// ============================================================
// VK_CHECK — VkResult 一括チェックヘルパー
//
// 戻り値を捨てていた Vulkan 呼び出し (acquire / present / fence / surface
// query 等) を一律に検査するためのユーティリティ (review/2026-06-11 Q-M3/M5/M6)。
//
//   VkResult r = VK_CHECK(vkSomeCall(...));   // 失敗時に call site + code を stderr へ
//   if (r != VK_SUCCESS) { ... }              // 分岐は呼び出し側の責務
//
// マクロは VkResult をそのまま評価値として返すので、 既存の戻り値分岐を壊さずに
// 「失敗時のログ出力」だけを差し込める。
// ============================================================

namespace pictor {

/// 代表的な VkResult を文字列化する (ログ用途、 網羅でなくてよい)。
inline const char* vk_result_string(VkResult r) {
    switch (r) {
        case VK_SUCCESS:                        return "VK_SUCCESS";
        case VK_NOT_READY:                      return "VK_NOT_READY";
        case VK_TIMEOUT:                        return "VK_TIMEOUT";
        case VK_SUBOPTIMAL_KHR:                 return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_SURFACE_LOST_KHR:         return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR:          return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_MEMORY_MAP_FAILED:        return "VK_ERROR_MEMORY_MAP_FAILED";
        default:                                return "VK_ERROR_<other>";
    }
}

/// 失敗時にのみ stderr へ call site + result code を出し、 result をそのまま返す。
inline VkResult vk_check_impl(VkResult r, const char* expr, const char* file, int line) {
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        std::fprintf(stderr, "[Pictor][VK] %s:%d  %s -> %s\n",
                     file, line, expr, vk_result_string(r));
    }
    return r;
}

} // namespace pictor

#define VK_CHECK(expr) ::pictor::vk_check_impl((expr), #expr, __FILE__, __LINE__)
