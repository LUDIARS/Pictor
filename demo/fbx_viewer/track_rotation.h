// Degree Euler → quaternion for track bone channels
// (SPEC-PC-FBX-TRACK-PLAYBACK). Intrinsic X, then Y, then Z: the composed
// rotation is Rx * Ry * Rz (Hamilton product), and it is applied on top of
// the bone's bind-pose local rotation as `bind_local * R`.
#pragma once

#include "pictor/animation/animation_types.h"

namespace pictor_fbx_viewer {

inline pictor::Quaternion track_rotation_xyz_deg(float rx, float ry, float rz) {
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    const pictor::Quaternion qx = pictor::Quaternion::from_axis_angle({1.0f, 0.0f, 0.0f}, rx * kDegToRad);
    const pictor::Quaternion qy = pictor::Quaternion::from_axis_angle({0.0f, 1.0f, 0.0f}, ry * kDegToRad);
    const pictor::Quaternion qz = pictor::Quaternion::from_axis_angle({0.0f, 0.0f, 1.0f}, rz * kDegToRad);
    return qx * qy * qz;
}

/// Bind-pose local transform with the track rotation composed in bone-local space.
inline pictor::Transform track_local_transform(const pictor::Transform& bind_local,
                                               float rx, float ry, float rz) {
    pictor::Transform t = bind_local;
    t.rotation = (bind_local.rotation * track_rotation_xyz_deg(rx, ry, rz)).normalized();
    return t;
}

} // namespace pictor_fbx_viewer
