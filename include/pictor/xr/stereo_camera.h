#pragma once

#include "pictor/core/camera.h"
#include "pictor/xr/xr_types.h"

namespace pictor::xr {

/// トラッキング空間をワールドへ置く「台座」。 観客の立ち位置と向きを host が決める
/// (VR 映画ならカット/移動撮影をここへ流し込む)。 頭の動きは ViewState 側が持つ。
struct CameraRig {
    float4x4 world_from_tracking = float4x4::identity();  // 剛体変換のみ
    float    near_z = 0.05f;
    float    far_z  = 1000.0f;
};

/// 両眼の ViewState から、 眼ごとの描画カメラとカリング用カメラを組み立てる。
///
/// カリング用カメラは両眼の視錐台をまとめて包む 1 つの視錐台で、 眼ごとに
/// カリングを 2 回走らせないために使う (描画には使わない)。
class StereoCamera {
public:
    void set_rig(const CameraRig& rig) { rig_ = rig; }
    const CameraRig& rig() const { return rig_; }

    /// 姿勢を反映する。 `views.is_valid == false` や視野・near/far が不正なら
    /// false を返し、 直前の有効なカメラを保つ (トラッキングが一瞬切れても
    /// 画が原点へ飛ばないようにするため)。
    bool update(const ViewState& views);

    /// 一度でも update が成功していれば true。
    bool has_valid_cameras() const { return has_valid_cameras_; }

    const Camera& eye(Eye e) const { return eyes_[static_cast<uint32_t>(e)]; }
    const Camera& culling_camera() const { return culling_; }

    /// 両眼の位置の距離 (メートル、 ワールド空間)。
    float eye_separation() const { return eye_separation_; }

private:
    CameraRig rig_;
    Camera    eyes_[kEyeCount];
    Camera    culling_;
    float     eye_separation_     = 0.0f;
    bool      has_valid_cameras_  = false;
};

} // namespace pictor::xr
