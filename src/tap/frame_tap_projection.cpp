#include "pictor/tap/frame_tap_projection.h"

#include "pictor/core/transform_math.h"

#include <algorithm>

namespace pictor {

namespace {

/// 視点の前とみなす w の下限。 w = 0 (視点面) での除算を避ける。
constexpr float kNearW = 1e-5f;

/// AABB の 12 辺 (8 隅の添字の組)。 隅の添字は bit0 = x, bit1 = y, bit2 = z (1 = max)。
constexpr int kEdges[12][2] = {
    {0, 1}, {2, 3}, {4, 5}, {6, 7},   // x 方向
    {0, 2}, {1, 3}, {4, 6}, {5, 7},   // y 方向
    {0, 4}, {1, 5}, {2, 6}, {3, 7},   // z 方向
};

struct ScreenAccumulator {
    float min_x = 0.0f, min_y = 0.0f, max_x = 0.0f, max_y = 0.0f;
    bool  any   = false;

    /// クリップ座標 (w > 0) を NDC → px に射影して外接矩形へ足す。
    /// Vulkan のクリップ空間は y 下向きなので NDC y = -1 が画面の上端。
    void add(const float4& clip, float width, float height) {
        const float sx = (clip.x / clip.w * 0.5f + 0.5f) * width;
        const float sy = (clip.y / clip.w * 0.5f + 0.5f) * height;
        if (!any) {
            min_x = max_x = sx;
            min_y = max_y = sy;
            any   = true;
            return;
        }
        min_x = std::min(min_x, sx);
        max_x = std::max(max_x, sx);
        min_y = std::min(min_y, sy);
        max_y = std::max(max_y, sy);
    }
};

float4 lerp(const float4& a, const float4& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}

} // namespace

bool project_world_aabb_to_screen(const AABB& bounds, const float4x4& view_proj,
                                  uint32_t viewport_width, uint32_t viewport_height,
                                  FrameTapScreenRect& out) {
    out = FrameTapScreenRect{};
    if (viewport_width == 0 || viewport_height == 0) return false;

    float4 clip[8];
    for (int i = 0; i < 8; ++i) {
        const float3 corner{(i & 1) ? bounds.max.x : bounds.min.x,
                            (i & 2) ? bounds.max.y : bounds.min.y,
                            (i & 4) ? bounds.max.z : bounds.min.z};
        clip[i] = transform_math::transform_point(view_proj, corner);
    }

    const float width  = static_cast<float>(viewport_width);
    const float height = static_cast<float>(viewport_height);
    ScreenAccumulator acc;

    for (const float4& c : clip) {
        if (c.w > kNearW) acc.add(c, width, height);
    }
    // 視点面をまたぐ辺は w = kNearW との交点を足す (隅だけだと後ろ側の広がりを落とす)。
    for (const auto& edge : kEdges) {
        const float4& a = clip[edge[0]];
        const float4& b = clip[edge[1]];
        const bool a_front = a.w > kNearW;
        const bool b_front = b.w > kNearW;
        if (a_front == b_front) continue;
        const float t = (kNearW - a.w) / (b.w - a.w);
        acc.add(lerp(a, b, t), width, height);
    }
    if (!acc.any) return false;

    const float x0 = std::clamp(acc.min_x, 0.0f, width);
    const float x1 = std::clamp(acc.max_x, 0.0f, width);
    const float y0 = std::clamp(acc.min_y, 0.0f, height);
    const float y1 = std::clamp(acc.max_y, 0.0f, height);
    if (!(x1 > x0) || !(y1 > y0)) return false;

    out = {x0, y0, x1 - x0, y1 - y0};
    return true;
}

float frame_tap_view_depth(const AABB& bounds, const float4x4& view) {
    const float3 center{(bounds.min.x + bounds.max.x) * 0.5f,
                        (bounds.min.y + bounds.max.y) * 0.5f,
                        (bounds.min.z + bounds.max.z) * 0.5f};
    return -transform_math::transform_point(view, center).z;
}

} // namespace pictor
