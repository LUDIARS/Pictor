#include "pictor/gi/gi_scene_proxy.h"

#include "pictor/scene/object_pool.h"

#include <cmath>
#include <limits>

namespace pictor {

namespace {

/// slab 法によるレイ-AABB 交差。 交差時は [t_near, t_far] を返す。
/// dir の成分が 0 の軸は 1/0 = ±inf の比較で自然に処理される
/// (IEEE754 前提 — min/max が inf を吸収する)。
bool ray_aabb(const float3& from, const float3& inv_dir, const AABB& box,
              float& t_near, float& t_far) {
    float t0 = (box.min.x - from.x) * inv_dir.x;
    float t1 = (box.max.x - from.x) * inv_dir.x;
    float tmin = std::min(t0, t1);
    float tmax = std::max(t0, t1);

    t0 = (box.min.y - from.y) * inv_dir.y;
    t1 = (box.max.y - from.y) * inv_dir.y;
    tmin = std::max(tmin, std::min(t0, t1));
    tmax = std::min(tmax, std::max(t0, t1));

    t0 = (box.min.z - from.z) * inv_dir.z;
    t1 = (box.max.z - from.z) * inv_dir.z;
    tmin = std::max(tmin, std::min(t0, t1));
    tmax = std::min(tmax, std::max(t0, t1));

    if (tmax < tmin) return false;
    t_near = tmin;
    t_far  = tmax;
    return true;
}

float3 safe_inverse(const float3& dir) {
    // 0 成分は ±inf にする (slab 法が想定する IEEE 挙動)。
    auto inv = [](float v) {
        return v != 0.0f ? 1.0f / v
                         : std::numeric_limits<float>::infinity();
    };
    return {inv(dir.x), inv(dir.y), inv(dir.z)};
}

} // namespace

void GISceneProxy::build(const AABB* boxes, const ObjectId* ids, uint32_t count) {
    boxes_.assign(boxes, boxes + count);
    ids_.assign(ids, ids + count);
}

void GISceneProxy::build(const ObjectPool& static_pool) {
    const uint32_t count = static_pool.count();
    boxes_.resize(count);
    ids_.resize(count);
    const auto& bounds = static_pool.bounds();
    const auto& ids    = static_pool.object_ids();
    for (uint32_t i = 0; i < count; ++i) {
        boxes_[i] = bounds[i];
        ids_[i]   = ids[i];
    }
}

void GISceneProxy::build(const ObjectPool& pool_a, const ObjectPool& pool_b) {
    const uint32_t ca = pool_a.count();
    const uint32_t cb = pool_b.count();
    boxes_.resize(static_cast<size_t>(ca) + cb);
    ids_.resize(static_cast<size_t>(ca) + cb);
    const auto& ba = pool_a.bounds();
    const auto& ia = pool_a.object_ids();
    for (uint32_t i = 0; i < ca; ++i) {
        boxes_[i] = ba[i];
        ids_[i]   = ia[i];
    }
    const auto& bb = pool_b.bounds();
    const auto& ib = pool_b.object_ids();
    for (uint32_t i = 0; i < cb; ++i) {
        boxes_[ca + i] = bb[i];
        ids_[ca + i]   = ib[i];
    }
}

bool GISceneProxy::occluded(const float3& from, const float3& dir,
                            float max_dist, ObjectId ignore) const {
    const float3 inv_dir = safe_inverse(dir);
    const uint32_t n = count();
    for (uint32_t i = 0; i < n; ++i) {
        if (ids_[i] == ignore) continue;
        float t_near = 0.0f, t_far = 0.0f;
        if (!ray_aabb(from, inv_dir, boxes_[i], t_near, t_far)) continue;
        // 前方 (t_far > 0) かつ max_dist 以内に交差区間がある。
        // t_near < 0 <= t_far は「レイ原点が箱の内側」 — 遮蔽扱いにしない
        // (自身を含む箱から外を見るケース。 ignore と別オブジェクトでも
        //  重なり配置で起きるため除外する)。
        if (t_near > 1e-4f && t_near < max_dist) return true;
    }
    return false;
}

GISceneProxy::Hit GISceneProxy::closest_hit(const float3& from, const float3& dir,
                                            float max_dist, ObjectId ignore) const {
    const float3 inv_dir = safe_inverse(dir);
    Hit best;
    float best_t = max_dist;
    const uint32_t n = count();
    for (uint32_t i = 0; i < n; ++i) {
        if (ids_[i] == ignore) continue;
        float t_near = 0.0f, t_far = 0.0f;
        if (!ray_aabb(from, inv_dir, boxes_[i], t_near, t_far)) continue;
        if (t_near > 1e-4f && t_near < best_t) {
            best_t        = t_near;
            best.hit      = true;
            best.distance = t_near;
            best.index    = i;
        }
    }
    return best;
}

} // namespace pictor
