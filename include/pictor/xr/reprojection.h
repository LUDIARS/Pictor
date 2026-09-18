#pragma once

#include "pictor/core/camera.h"

namespace pictor::xr {

/// 描画済みの画 (src) を別の視点 (dst) へ写すための行列を作る。
///
/// 結果は「src の NDC 座標 (x, y, 深度) → dst のクリップ座標」で、 行ベクトル規約
/// (dst_clip = src_ndc * out)。 シェーダへはそのままのメモリ配置で渡せる。
/// 両眼差分 (左眼 → 右眼) と時間差分 (キーフレーム → 現在) の両方で使う。
///
/// src の view * projection が逆行列を持たなければ false。
bool build_reprojection(const Camera& src, const Camera& dst, float4x4& out);

/// 2 つのカメラの視点の隔たり。 キーフレームを使い回せるかの判断材料。
struct ViewDelta {
    float translation_m = 0.0f;  // 視点位置の距離
    float rotation_rad  = 0.0f;  // 視線方向のなす角
};

ViewDelta measure_view_delta(const Camera& a, const Camera& b);

} // namespace pictor::xr
