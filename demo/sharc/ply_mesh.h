#pragma once

/// PLY メッシュ読み込み (SHaRC デモローカル)。
///
/// Stanford 3D Scanning Repository の vrip 再構成 PLY
/// (ascii / binary_little_endian、 位置 + 面リスト、 余分な property は
/// 読み飛ばし) を対象にした最小ローダ。 スムーズ法線 (面積重み) を計算し、
/// D2 逆光透過デモが SSS 評価に使う。
///
/// SRP: ファイル読み込みと法線計算のみ。 空間構造 (BVH) は mesh_bvh.h。

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sharc_demo {

struct PlyMesh {
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;    ///< 頂点スムーズ法線
    std::vector<std::array<uint32_t, 3>> triangles;

    std::array<float, 3> bounds_min{};
    std::array<float, 3> bounds_max{};

    bool empty() const { return triangles.empty(); }
};

/// PLY を読み込み、 スムーズ法線と AABB を計算して返す。
/// 失敗時は empty() な PlyMesh (理由は stderr へ)。
PlyMesh load_ply(const std::string& path);

/// メッシュを一様スケール + 平行移動する (デモのワールド配置用)。
/// target_extent = 最大軸の長さ (m)、 底面 y=0 / x=z=0 中心に置く。
void fit_mesh(PlyMesh& mesh, float target_extent);

}  // namespace sharc_demo
