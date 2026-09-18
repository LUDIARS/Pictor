#include "pictor/xr/reprojection.h"

#include "pictor/core/transform_math.h"

#include <algorithm>
#include <cmath>

namespace pictor::xr {

/// @implements SPEC-PC-XR-DELTA-RENDERING
bool build_reprojection(const Camera& src, const Camera& dst, float4x4& out) {
    float4x4 world_from_src_ndc;
    if (!transform_math::inverse(
            transform_math::multiply(src.view, src.projection), world_from_src_ndc)) {
        return false;
    }
    out = transform_math::multiply(
        world_from_src_ndc, transform_math::multiply(dst.view, dst.projection));
    return true;
}

/// @implements SPEC-PC-XR-DELTA-RENDERING
ViewDelta measure_view_delta(const Camera& a, const Camera& b) {
    ViewDelta d;
    const float3 t = a.position - b.position;
    d.translation_m = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);

    // view 行列の第 3 列は、 ワールド座標を視点空間の z へ写す係数 = 視点の +z 軸
    // (後ろ向き) をワールドで表したもの。 向きの比較にはこれで足りる。
    const float3 za{a.view.m[0][2], a.view.m[1][2], a.view.m[2][2]};
    const float3 zb{b.view.m[0][2], b.view.m[1][2], b.view.m[2][2]};
    const float dot = za.x * zb.x + za.y * zb.y + za.z * zb.z;
    d.rotation_rad = std::acos(std::clamp(dot, -1.0f, 1.0f));
    return d;
}

} // namespace pictor::xr
