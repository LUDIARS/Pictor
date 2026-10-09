#pragma once

/// world AABB の画面投影と視点深度 (spec/feature/frame-tap.md §5)。
///
/// 頂点は走査しない。 AABB の 8 隅と 12 辺だけを使う。

#include "pictor/core/types.h"
#include "pictor/tap/frame_tap_types.h"

#include <cstdint>

namespace pictor {

/// `view_proj` (= view * projection、 行ベクトル規約) で AABB を投影し、 viewport
/// で切り詰めた外接矩形を `out` に書く。 視点の後ろへまたがる箱は w = ε 面で
/// 辺を切ってから射影する。 面積が残れば true、 画面外なら false で out は 0。
bool project_world_aabb_to_screen(const AABB& bounds, const float4x4& view_proj,
                                  uint32_t viewport_width, uint32_t viewport_height,
                                  FrameTapScreenRect& out);

/// AABB 中心の視点からの深度 (視点空間は -z が前方なので -z_view)。
float frame_tap_view_depth(const AABB& bounds, const float4x4& view);

} // namespace pictor
