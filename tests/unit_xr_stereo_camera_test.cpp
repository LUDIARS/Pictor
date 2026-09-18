// StereoCamera / 両眼を包むカリング視錐台の契約。

#include "pictor/core/transform_math.h"
#include "pictor/xr/stereo_camera.h"
#include "test_common.h"

#include <cmath>

using namespace pictor;
using namespace pictor::xr;
namespace xf = pictor::transform_math;

namespace {

constexpr float kIpd = 0.064f;

bool feq(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

/// Quest に近い視野。 内側 (鼻側) が外側より狭い非対称。
ViewState make_views(float head_x = 0.0f) {
    ViewState v;
    v.is_valid = true;
    v.eyes[0].pose.position = {head_x - kIpd * 0.5f, 1.6f, 0.0f};
    v.eyes[1].pose.position = {head_x + kIpd * 0.5f, 1.6f, 0.0f};
    v.eyes[0].fov = {-0.94f, 0.70f, 0.84f, -0.96f};
    v.eyes[1].fov = {-0.70f, 0.94f, 0.84f, -0.96f};
    return v;
}

AABB box_at(const float3& c, float half) {
    AABB b;
    b.min = {c.x - half, c.y - half, c.z - half};
    b.max = {c.x + half, c.y + half, c.z + half};
    return b;
}

void test_eye_cameras_follow_pose() {
    StereoCamera cam;
    PT_ASSERT(!cam.has_valid_cameras(), "no cameras before the first update");
    PT_ASSERT(cam.update(make_views()), "valid views are accepted");
    PT_ASSERT(cam.has_valid_cameras(), "cameras are valid after update");

    PT_ASSERT(feq(cam.eye(Eye::Left).position.x, -kIpd * 0.5f), "left eye sits at -ipd/2");
    PT_ASSERT(feq(cam.eye(Eye::Right).position.x, kIpd * 0.5f), "right eye sits at +ipd/2");
    PT_ASSERT(feq(cam.eye_separation(), kIpd), "eye separation equals the ipd");

    // 視点そのものは視点空間の原点へ写る。
    const float4 origin = xf::transform_point(cam.eye(Eye::Left).view,
                                              cam.eye(Eye::Left).position);
    PT_ASSERT(feq(origin.x, 0.0f) && feq(origin.y, 0.0f) && feq(origin.z, 0.0f),
              "view matrix maps the eye position to the origin");
}

void test_rig_moves_the_viewer() {
    StereoCamera cam;
    CameraRig rig;
    rig.world_from_tracking.set_translation(100.0f, 0.0f, -50.0f);
    cam.set_rig(rig);
    PT_ASSERT(cam.update(make_views()), "update with a moved rig");
    PT_ASSERT(feq(cam.eye(Eye::Left).position.x, 100.0f - kIpd * 0.5f, 1e-3f),
              "rig translation carries the eye in x");
    PT_ASSERT(feq(cam.eye(Eye::Left).position.z, -50.0f, 1e-3f),
              "rig translation carries the eye in z");
}

void test_invalid_views_keep_last_cameras() {
    StereoCamera cam;
    PT_ASSERT(cam.update(make_views(1.0f)), "first update");
    const float before = cam.eye(Eye::Left).position.x;

    ViewState lost = make_views(5.0f);
    lost.is_valid = false;
    PT_ASSERT(!cam.update(lost), "untracked views are rejected");
    PT_ASSERT(feq(cam.eye(Eye::Left).position.x, before), "last valid camera is kept");

    ViewState broken = make_views(5.0f);
    broken.eyes[1].fov.angle_right = broken.eyes[1].fov.angle_left;  // 幅 0 の視野
    PT_ASSERT(!cam.update(broken), "degenerate fov is rejected");
    PT_ASSERT(feq(cam.eye(Eye::Left).position.x, before),
              "a half-built frame never replaces the cameras");
}

void test_culling_frustum_contains_both_eyes() {
    StereoCamera cam;
    PT_ASSERT(cam.update(make_views()), "update");
    const Frustum& cull  = cam.culling_camera().frustum;
    const Frustum& left  = cam.eye(Eye::Left).frustum;
    const Frustum& right = cam.eye(Eye::Right).frustum;

    // どちらかの眼に見える箱は、 必ずカリング視錐台にも入る (見えるものを落とさない)。
    int visible = 0;
    for (float z = -0.2f; z > -60.0f; z *= 1.7f) {
        for (float x = -2.0f; x <= 2.0f; x += 0.25f) {
            for (float y = -1.5f; y <= 1.5f; y += 0.5f) {
                const AABB box = box_at({x * -z, 1.6f + y * -z, z}, 0.01f);
                if (!left.test_aabb(box) && !right.test_aabb(box)) continue;
                ++visible;
                PT_ASSERT(cull.test_aabb(box), "box seen by an eye is inside the culling frustum");
            }
        }
    }
    PT_ASSERT(visible > 50, "the sweep actually covered visible boxes");

    // 真後ろの箱は落とす (何でも通す視錐台になっていないこと)。
    PT_ASSERT(!cull.test_aabb(box_at({0.0f, 1.6f, 5.0f}, 0.1f)), "box behind the head is culled");
    // 大きく横に外れた箱も落とす。
    PT_ASSERT(!cull.test_aabb(box_at({-80.0f, 1.6f, -10.0f}, 0.1f)), "box far to the side is culled");
}

} // namespace

int main() {
    test_eye_cameras_follow_pose();
    test_rig_moves_the_viewer();
    test_invalid_views_keep_last_cameras();
    test_culling_frustum_contains_both_eyes();
    return pictor_test::report("unit_xr_stereo_camera_test");
}
