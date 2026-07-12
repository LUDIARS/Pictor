#pragma once

/// GISceneProxy — GI ベイク / プローブ構築が共有する遮蔽クエリ。
///
/// Pictor はメッシュ (三角形) を所有しない (host-driven 原則) ため、
/// 遮蔽判定は静的オブジェクトの AABB プロキシに対するレイキャストで行う
/// (`spec/feature/gi-bake-realtime-design.md` §2.1)。 精密な三角形遮蔽は
/// phase 3 (`IBakeDataProvider` 拡張) の領分。
///
/// build() で static pool の AABB を flat 配列へ写し取り (init 1 回コピー、
/// 以降 alloc なし — DoD 規約)、 occluded() / closest_hit() は全件走査の
/// slab test。 数千オブジェクト × 数百レイのオフライン用途に十分。

#include "pictor/core/types.h"

#include <vector>

namespace pictor {

class ObjectPool;

class GISceneProxy {
public:
    /// レイの最近ヒット結果。 `hit == false` のとき他フィールドは無効。
    struct Hit {
        bool     hit      = false;
        float    distance = 0.0f;
        uint32_t index    = 0;      ///< プロキシ内 index (`object_id()` で引く)
    };

    /// 生配列から構築する (テスト / ホスト供給データ用)。
    void build(const AABB* boxes, const ObjectId* ids, uint32_t count);

    /// static pool の bounds / object_ids から構築する。
    void build(const ObjectPool& static_pool);

    /// `from` から `dir` (正規化済み) 方向 `max_dist` 以内に遮蔽物があるか。
    /// `ignore` の ObjectId は自己遮蔽防止のためスキップする。 any-hit。
    bool occluded(const float3& from, const float3& dir,
                  float max_dist, ObjectId ignore = INVALID_OBJECT_ID) const;

    /// 最近ヒット (closest-hit)。 バウンス面の推定に使う。
    Hit closest_hit(const float3& from, const float3& dir,
                    float max_dist, ObjectId ignore = INVALID_OBJECT_ID) const;

    uint32_t count() const { return static_cast<uint32_t>(boxes_.size()); }
    bool     empty() const { return boxes_.empty(); }

    const AABB& box(uint32_t index) const { return boxes_[index]; }
    ObjectId object_id(uint32_t index) const { return ids_[index]; }

private:
    // flat SoA — build で 1 回確保、 クエリ中は不変。
    std::vector<AABB>     boxes_;
    std::vector<ObjectId> ids_;
};

} // namespace pictor
