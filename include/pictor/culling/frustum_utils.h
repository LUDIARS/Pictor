#pragma once

#include "pictor/core/types.h"

namespace pictor::frustum_utils {

/// 視野行列 × 投影行列から視錐台の 6 平面を取り出す (内向き法線、 正規化済み)。
/// 平面の並びは Frustum の規約どおり left, right, bottom, top, near, far。
/// クリップ空間は Vulkan (z は [0,1]) を前提にする。
Frustum extract_frustum(const float4x4& vp);

} // namespace pictor::frustum_utils
