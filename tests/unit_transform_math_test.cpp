// transform_math: 行列の積・逆行列・姿勢・非対称投影の契約。

#include "pictor/core/transform_math.h"
#include "test_common.h"

#include <cmath>

using namespace pictor;
namespace xf = pictor::transform_math;

namespace {

bool feq(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

bool is_identity(const float4x4& m, float eps = 1e-4f) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (!feq(m.m[i][j], i == j ? 1.0f : 0.0f, eps)) return false;
    return true;
}

// y 軸まわり 90 度。 -z (前方) が -x を向く。
float4 quarter_turn_y() {
    const float h = std::sqrt(0.5f);
    return {0.0f, h, 0.0f, h};
}

void test_inverse_roundtrip() {
    const float4x4 pose = xf::from_pose(quarter_turn_y(), {1.0f, 2.0f, 3.0f});
    float4x4 proj;
    PT_ASSERT(xf::perspective_asymmetric(-1.2f, 0.9f, 1.0f, -1.1f, 0.05f, 100.0f, proj),
              "valid projection builds");
    const float4x4 m = xf::multiply(pose, proj);

    float4x4 inv;
    PT_ASSERT(xf::inverse(m, inv), "view-projection is invertible");
    PT_ASSERT(is_identity(xf::multiply(m, inv), 1e-3f), "m * inverse(m) == identity");

    float4x4 untouched = float4x4::identity();
    untouched.m[0][0] = 7.0f;
    PT_ASSERT(!xf::inverse(float4x4{}, untouched), "zero matrix is singular");
    PT_ASSERT(feq(untouched.m[0][0], 7.0f), "singular input leaves out untouched");
}

void test_rigid_inverse_matches_general() {
    const float4x4 pose = xf::from_pose(quarter_turn_y(), {0.5f, -1.0f, 4.0f});
    PT_ASSERT(is_identity(xf::multiply(pose, xf::inverse_rigid(pose))),
              "pose * inverse_rigid(pose) == identity");
}

void test_pose_orientation_and_position() {
    const float4x4 pose = xf::from_pose(quarter_turn_y(), {10.0f, 0.0f, 0.0f});
    // ローカルの前方 (0,0,-1) は、 y 軸 90 度回転で (-1,0,0) を向く。
    const float4 fwd = xf::transform_point(pose, {0.0f, 0.0f, -1.0f});
    PT_ASSERT(feq(fwd.x, 9.0f) && feq(fwd.y, 0.0f) && feq(fwd.z, 0.0f),
              "forward rotates to -x and is offset by the position");

    // 長さ 0 の四元数は無回転として扱う。
    const float4x4 safe = xf::from_pose({0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f});
    PT_ASSERT(is_identity(safe), "zero quaternion falls back to identity rotation");
}

void test_asymmetric_projection_maps_edges() {
    const float l = -1.5f, r = 0.8f, u = 1.1f, d = -0.9f, n = 0.1f, f = 50.0f;
    float4x4 proj;
    PT_ASSERT(xf::perspective_asymmetric(l, r, u, d, n, f, proj), "builds");

    auto ndc = [&](float tx, float ty, float depth) {
        const float4 c = xf::transform_point(proj, {tx * depth, ty * depth, -depth});
        return float3{c.x / c.w, c.y / c.w, c.z / c.w};
    };
    const float3 left_top_near = ndc(l, u, n);
    PT_ASSERT(feq(left_top_near.x, -1.0f), "left edge maps to x = -1");
    PT_ASSERT(feq(left_top_near.y, -1.0f), "top edge maps to y = -1 (Vulkan is y-down)");
    PT_ASSERT(feq(left_top_near.z, 0.0f), "near plane maps to z = 0");

    const float3 right_bottom_far = ndc(r, d, f);
    PT_ASSERT(feq(right_bottom_far.x, 1.0f), "right edge maps to x = +1");
    PT_ASSERT(feq(right_bottom_far.y, 1.0f), "bottom edge maps to y = +1");
    PT_ASSERT(feq(right_bottom_far.z, 1.0f, 1e-3f), "far plane maps to z = 1");
}

void test_projection_rejects_bad_input() {
    float4x4 proj;
    PT_ASSERT(!xf::perspective_asymmetric(0.5f, -0.5f, 1.0f, -1.0f, 0.1f, 10.0f, proj),
              "right <= left is rejected");
    PT_ASSERT(!xf::perspective_asymmetric(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 10.0f, proj),
              "up <= down is rejected");
    PT_ASSERT(!xf::perspective_asymmetric(-1.0f, 1.0f, 1.0f, -1.0f, 0.0f, 10.0f, proj),
              "near <= 0 is rejected");
    PT_ASSERT(!xf::perspective_asymmetric(-1.0f, 1.0f, 1.0f, -1.0f, 5.0f, 5.0f, proj),
              "far <= near is rejected");
}

} // namespace

int main() {
    test_inverse_roundtrip();
    test_rigid_inverse_matches_general();
    test_pose_orientation_and_position();
    test_asymmetric_projection_maps_edges();
    test_projection_rejects_bad_input();
    return pictor_test::report("unit_transform_math_test");
}
