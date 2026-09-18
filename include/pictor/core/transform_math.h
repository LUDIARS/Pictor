#pragma once

#include "pictor/core/types.h"

namespace pictor {

/// 行列と姿勢の基本演算。
///
/// 規約 (Pictor 全体と同じ): 行ベクトル (clip = p * view * proj)、 `m[row][col]`、
/// 平行移動は `m[3][0..2]`、 視点空間は右手系で -z が前方、 クリップ空間は
/// Vulkan (y 下向き、 z は [0,1])。
namespace transform_math {

float4x4 multiply(const float4x4& a, const float4x4& b);

/// 一般の 4x4 逆行列。 特異なら false を返し `out` は書き換えない。
bool inverse(const float4x4& m, float4x4& out);

/// 回転 + 平行移動のみの行列の逆行列 (スケール・せん断を含まない前提)。
float4x4 inverse_rigid(const float4x4& m);

/// 姿勢 (回転 + 位置) から「ローカル → 親」の剛体変換行列を作る。
/// `orientation_xyzw` は四元数 (x, y, z, w)。 正規化して使い、 長さ 0 は無回転として扱う。
/// animation モジュールの Quaternion には依存しない (任意モジュールのため)。
float4x4 from_pose(const float4& orientation_xyzw, const float3& position);

/// 左右上下で非対称な視野の透視投影。 引数は視線軸からの角度の正接で、
/// 左と下は負、 右と上は正 (OpenXR の XrFovf と同じ符号)。
/// `right <= left` / `up <= down` / `near_z <= 0` / `far_z <= near_z` は
/// 不正入力として false を返す。
bool perspective_asymmetric(float tan_left, float tan_right,
                            float tan_up, float tan_down,
                            float near_z, float far_z, float4x4& out);

/// 点 (w = 1) を行ベクトル規約で変換し、 w 除算前の同次座標を返す。
float4 transform_point(const float4x4& m, const float3& p);

} // namespace transform_math
} // namespace pictor
