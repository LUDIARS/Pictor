#include "pictor/xr/stereo_camera.h"

#include "pictor/core/transform_math.h"
#include "pictor/culling/frustum_utils.h"
#include "pictor/xr/stereo_frustum.h"

#include <cmath>

namespace pictor::xr {

namespace {

bool build_eye_camera(const CameraRig& rig, const EyeView& view, Camera& out) {
    float4x4 projection;
    if (!transform_math::perspective_asymmetric(
            std::tan(view.fov.angle_left), std::tan(view.fov.angle_right),
            std::tan(view.fov.angle_up), std::tan(view.fov.angle_down),
            rig.near_z, rig.far_z, projection)) {
        return false;
    }

    // 眼 → トラッキング空間 → ワールド (行ベクトル規約なので左から順に掛ける)。
    const float4x4 world_from_eye = transform_math::multiply(
        transform_math::from_pose(view.pose.orientation, view.pose.position),
        rig.world_from_tracking);

    out.view       = transform_math::inverse_rigid(world_from_eye);
    out.projection = projection;
    out.position   = world_from_eye.get_translation();
    out.frustum    = frustum_utils::extract_frustum(
        transform_math::multiply(out.view, out.projection));
    return true;
}

} // namespace

bool StereoCamera::update(const ViewState& views) {
    if (!views.is_valid) return false;

    // 途中で失敗しても直前のカメラを壊さないよう、 一時領域で組み立ててから差し替える。
    Camera eyes[kEyeCount];
    for (uint32_t i = 0; i < kEyeCount; ++i) {
        if (!build_eye_camera(rig_, views.eyes[i], eyes[i])) return false;
    }
    Camera culling;
    if (!build_combined_culling_camera(eyes[0], eyes[1], culling)) return false;

    const float3 d = eyes[1].position - eyes[0].position;
    eye_separation_ = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    eyes_[0] = eyes[0];
    eyes_[1] = eyes[1];
    culling_ = culling;
    has_valid_cameras_ = true;
    return true;
}

} // namespace pictor::xr
