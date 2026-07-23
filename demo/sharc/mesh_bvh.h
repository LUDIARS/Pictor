#pragma once

/// 三角形メッシュ BVH (SHaRC デモローカル)。
///
/// CPU 一次レイの解析交差用。 中央値分割 (最長軸) + Möller–Trumbore。
/// D2 デモは 320x180 = 57,600 レイ/フレームをこの BVH に投げる。
///
/// SRP: 空間構造と交差判定のみ。 メッシュ読み込みは ply_mesh.h。

#include "ply_mesh.h"

#include <cstdint>
#include <vector>

namespace sharc_demo {

struct BvhHit {
    float t = -1.0f;
    float normal[3] = {0.0f, 1.0f, 0.0f};   ///< スムーズ法線 (重心補間)
    bool valid() const { return t > 0.0f; }
};

class MeshBvh {
public:
    /// mesh は借用 (呼び出し側が寿命を保証)。 build 後に交差可能。
    void build(const PlyMesh& mesh);

    bool is_built() const { return !nodes_.empty(); }

    /// 最近交差。 tmax より遠いヒットは無視。
    BvhHit intersect(const float ro[3], const float rd[3], float tmax) const;

private:
    struct Node {
        float    bmin[3];
        float    bmax[3];
        uint32_t left  = 0;   ///< 内部: 子 index / 葉: 三角形開始
        uint32_t count = 0;   ///< 0 = 内部ノード / >0 = 葉の三角形数
    };

    uint32_t build_recursive_(std::vector<uint32_t>& tris, uint32_t begin,
                              uint32_t end, int depth);

    const PlyMesh* mesh_ = nullptr;
    std::vector<Node>     nodes_;
    std::vector<uint32_t> tri_order_;   ///< 葉が参照する三角形 index 列
};

}  // namespace sharc_demo
