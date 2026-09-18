// 再投影行列と視点差の契約。

#include "pictor/core/transform_math.h"
#include "pictor/xr/reprojection.h"
#include "pictor/xr/stereo_camera.h"
#include "test_common.h"

#include <cmath>

using namespace pictor;
using namespace pictor::xr;
namespace xf = pictor::transform_math;

namespace {

bool feq(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

StereoCamera make_stereo() {
    ViewState v;
    v.is_valid = true;
    v.eyes[0].pose.position = {-0.032f, 0.0f, 0.0f};
    v.eyes[1].pose.position = { 0.032f, 0.0f, 0.0f};
    v.eyes[0].fov = {-0.94f, 0.70f, 0.84f, -0.96f};
    v.eyes[1].fov = {-0.70f, 0.94f, 0.84f, -0.96f};
    StereoCamera cam;
    cam.update(v);
    return cam;
}

float3 to_ndc(const Camera& cam, const float3& world) {
    const float4 c = xf::transform_point(xf::multiply(cam.view, cam.projection), world);
    return {c.x / c.w, c.y / c.w, c.z / c.w};
}

void test_same_camera_is_identity() {
    const StereoCamera stereo = make_stereo();
    float4x4 m;
    PT_ASSERT(build_reprojection(stereo.eye(Eye::Left), stereo.eye(Eye::Left), m), "builds");

    const float3 src{0.3f, -0.4f, 0.7f};
    const float4 dst = xf::transform_point(m, src);
    PT_ASSERT(feq(dst.x / dst.w, src.x) && feq(dst.y / dst.w, src.y) && feq(dst.z / dst.w, src.z),
              "reprojecting onto the same camera keeps every ndc point in place");
}

void test_left_to_right_matches_direct_projection() {
    const StereoCamera stereo = make_stereo();
    const Camera& left  = stereo.eye(Eye::Left);
    const Camera& right = stereo.eye(Eye::Right);
    float4x4 m;
    PT_ASSERT(build_reprojection(left, right, m), "builds");

    // 左眼で見えた点を再投影した先は、 その点を右眼で直接投影した先と一致する。
    const float3 points[] = {{0.1f, 0.2f, -1.0f}, {-0.5f, -0.3f, -3.0f}, {2.0f, 1.0f, -40.0f}};
    for (const float3& p : points) {
        const float3 in_left  = to_ndc(left, p);
        const float3 expected = to_ndc(right, p);
        const float4 warped   = xf::transform_point(m, in_left);
        PT_ASSERT(feq(warped.x / warped.w, expected.x), "x matches the direct projection");
        PT_ASSERT(feq(warped.y / warped.w, expected.y), "y matches the direct projection");
        PT_ASSERT(feq(warped.z / warped.w, expected.z), "depth matches the direct projection");
    }

    // 近い点ほど左右の眼でのずれ (視差) が大きい。
    const float near_shift = to_ndc(left, points[0]).x - to_ndc(right, points[0]).x;
    const float far_shift  = to_ndc(left, points[2]).x - to_ndc(right, points[2]).x;
    PT_ASSERT(near_shift > far_shift && far_shift > 0.0f, "parallax shrinks with distance");
}

void test_singular_source_is_rejected() {
    const StereoCamera stereo = make_stereo();
    Camera broken = stereo.eye(Eye::Left);
    broken.projection = float4x4{};
    float4x4 m;
    PT_ASSERT(!build_reprojection(broken, stereo.eye(Eye::Right), m),
              "a source without an invertible view-projection is rejected");
}

void test_view_delta() {
    const StereoCamera stereo = make_stereo();
    const ViewDelta eyes = measure_view_delta(stereo.eye(Eye::Left), stereo.eye(Eye::Right));
    PT_ASSERT(feq(eyes.translation_m, 0.064f), "eyes are one ipd apart");
    PT_ASSERT(feq(eyes.rotation_rad, 0.0f), "parallel eyes have no rotation between them");

    // y 軸まわり 30 度だけ振り向く。
    const float half = 0.5f * 0.5235988f;
    ViewState turned;
    turned.is_valid = true;
    for (uint32_t i = 0; i < kEyeCount; ++i) {
        turned.eyes[i].pose.orientation = {0.0f, std::sin(half), 0.0f, std::cos(half)};
        turned.eyes[i].fov = {-0.8f, 0.8f, 0.8f, -0.8f};
    }
    turned.eyes[1].pose.position = {0.064f, 0.0f, 0.0f};
    StereoCamera after;
    PT_ASSERT(after.update(turned), "turned views are valid");
    const ViewDelta d = measure_view_delta(stereo.eye(Eye::Left), after.eye(Eye::Left));
    PT_ASSERT(feq(d.rotation_rad, 0.5235988f), "a 30 degree turn measures 30 degrees");
}

} // namespace

int main() {
    test_same_camera_is_identity();
    test_left_to_right_matches_direct_projection();
    test_singular_source_is_rejected();
    test_view_delta();
    return pictor_test::report("unit_xr_reprojection_test");
}
