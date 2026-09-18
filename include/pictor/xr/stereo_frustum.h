#pragma once

#include "pictor/core/camera.h"

namespace pictor::xr {

/// 両眼の視錐台をまとめて包む 1 つのカメラを作る。
///
/// 左眼の座標系で、 視点を両眼の中点から後ろへ下げた透視視錐台を取り、 両眼の
/// 視錐台の頂点 16 個がすべて入るよう視野を広げる。 視錐台は凸なので、 頂点が
/// 入れば両眼の視錐台の全体が入る (保守的 = 見えるものを落とさない)。
/// 左右の表示面が内外へ傾いた HMD でも成り立つ。
///
/// 眼のカメラの view / projection が逆行列を持たない場合は false。
bool build_combined_culling_camera(const Camera& left, const Camera& right,
                                   Camera& out);

} // namespace pictor::xr
