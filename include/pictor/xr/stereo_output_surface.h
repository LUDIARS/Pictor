#pragma once

#include "pictor/xr/xr_types.h"
#include <cstdint>

#ifdef PICTOR_HAS_VULKAN
#ifndef NOGDI
#define NOGDI
#endif
#include <vulkan/vulkan.h>
#endif

namespace pictor::xr {

/// HMD へ提出する両眼ぶんの描画先。 1 枚の画像が 2 層 (層 0 = 左眼、 層 1 = 右眼) を持つ。
///
/// 1 フレームの呼び出し順: acquire → (描画) → release。 release した画像が
/// そのフレームの IXrSession::end_frame で提出される。
/// 画像と view は実装が所有する。 host は破棄しない。
class IStereoOutputSurface {
public:
    virtual ~IStereoOutputSurface() = default;

    virtual EyeExtent eye_extent() const = 0;
    virtual uint32_t  image_count() const = 0;

    /// 次に描く画像を確保し、 描き込めるようになるまで待つ。
    virtual bool acquire(uint32_t& out_image_index) = 0;
    virtual bool release() = 0;

#ifdef PICTOR_HAS_VULKAN
    virtual VkFormat color_format() const = 0;
    /// 片眼ぶんの 2D view (眼ごとに描く方式・再投影の写し先)。
    virtual VkImageView eye_view(uint32_t image_index, Eye eye) const = 0;
    /// 2 層まとめた 2D_ARRAY view (multiview の描画先)。
    virtual VkImageView array_view(uint32_t image_index) const = 0;
#endif
};

} // namespace pictor::xr
