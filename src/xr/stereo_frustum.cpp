#include "pictor/xr/stereo_frustum.h"

#include "pictor/core/transform_math.h"
#include "pictor/culling/frustum_utils.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace pictor::xr {

namespace {

constexpr int   kCornersPerFrustum = 8;
// 視野の外向きの正接がこれより小さい投影は異常値として扱い、 視点を下げる量が
// 発散しないようにする。
constexpr float kMinOutwardTan = 0.1f;
// 頂点はちょうど境界に乗るので、 丸め誤差で外へこぼれないよう少し広げる。
constexpr float kTanPadding   = 1.001f;
constexpr float kDepthPadding = 1.001f;
constexpr float kMinDepth     = 1e-5f;

/// 眼のカメラの視錐台の頂点 8 個をワールド空間で求める。
bool frustum_corners_world(const Camera& cam, float3 out[kCornersPerFrustum]) {
    float4x4 inv_vp;
    if (!transform_math::inverse(
            transform_math::multiply(cam.view, cam.projection), inv_vp)) {
        return false;
    }
    int n = 0;
    for (float z : {0.0f, 1.0f}) {
        for (float y : {-1.0f, 1.0f}) {
            for (float x : {-1.0f, 1.0f}) {
                const float4 h = transform_math::transform_point(inv_vp, {x, y, z});
                if (std::fabs(h.w) < kMinDepth) return false;
                const float inv_w = 1.0f / h.w;
                out[n++] = {h.x * inv_w, h.y * inv_w, h.z * inv_w};
            }
        }
    }
    return true;
}

float3 to_space(const float4x4& m, const float3& p) {
    const float4 h = transform_math::transform_point(m, p);
    return {h.x, h.y, h.z};
}

} // namespace

bool build_combined_culling_camera(const Camera& left, const Camera& right,
                                   Camera& out) {
    float3 corners[kCornersPerFrustum * 2];
    if (!frustum_corners_world(left, corners)) return false;
    if (!frustum_corners_world(right, corners + kCornersPerFrustum)) return false;

    // 作業空間 = 左眼の視点空間 (左眼が原点、 -z が前方)。
    const float3 right_eye = to_space(left.view, right.position);
    const float  separation = std::sqrt(right_eye.x * right_eye.x +
                                        right_eye.y * right_eye.y +
                                        right_eye.z * right_eye.z);

    // 外向きの視野 (左眼の左、 右眼の右) のうち狭い方に合わせて視点を下げる。
    // 下げる量 d = separation / (2 tan) のとき、 near 側の頂点と far 側の頂点が
    // 同じ視野角に収まり、 包む視錐台が最も細くなる。
    const float left_tan  = std::fabs((left.projection.m[2][0] - 1.0f) /
                                      left.projection.m[0][0]);
    const float right_tan = std::fabs((right.projection.m[2][0] + 1.0f) /
                                      right.projection.m[0][0]);
    const float outward_tan = std::max(kMinOutwardTan, std::min(left_tan, right_tan));
    const float pull_back   = separation / (2.0f * outward_tan);

    const float3 apex{right_eye.x * 0.5f, right_eye.y * 0.5f,
                      right_eye.z * 0.5f + pull_back};

    float tan_l = 0.0f, tan_r = 0.0f, tan_d = 0.0f, tan_u = 0.0f;
    float near_depth = 0.0f, far_depth = 0.0f;
    for (int i = 0; i < kCornersPerFrustum * 2; ++i) {
        const float3 q = to_space(left.view, corners[i]) - apex;
        const float depth = -q.z;
        if (depth < kMinDepth) return false;
        const float tx = q.x / depth;
        const float ty = q.y / depth;
        if (i == 0) {
            tan_l = tan_r = tx;
            tan_d = tan_u = ty;
            near_depth = far_depth = depth;
            continue;
        }
        tan_l = std::min(tan_l, tx);  tan_r = std::max(tan_r, tx);
        tan_d = std::min(tan_d, ty);  tan_u = std::max(tan_u, ty);
        near_depth = std::min(near_depth, depth);
        far_depth  = std::max(far_depth, depth);
    }

    // 0 をまたがない側へ広げると逆に狭まるので、 中心からの幅を広げる。
    const float cx = (tan_l + tan_r) * 0.5f, hx = (tan_r - tan_l) * 0.5f * kTanPadding;
    const float cy = (tan_d + tan_u) * 0.5f, hy = (tan_u - tan_d) * 0.5f * kTanPadding;

    float4x4 projection;
    if (!transform_math::perspective_asymmetric(cx - hx, cx + hx, cy + hy, cy - hy,
                                                near_depth / kDepthPadding,
                                                far_depth * kDepthPadding,
                                                projection)) {
        return false;
    }

    float4x4 shift = float4x4::identity();
    shift.set_translation(-apex.x, -apex.y, -apex.z);

    out.view       = transform_math::multiply(left.view, shift);
    out.projection = projection;
    out.position   = to_space(transform_math::inverse_rigid(left.view), apex);
    out.frustum    = frustum_utils::extract_frustum(
        transform_math::multiply(out.view, out.projection));
    return true;
}

} // namespace pictor::xr
