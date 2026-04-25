/// Frustum culling unit test: AABB-frustum classification (§10.1).
///
/// Builds an axis-aligned cube frustum [-10..+10]^3 and verifies inside,
/// outside, and straddling AABBs are classified as expected.

#include "pictor/core/types.h"
#include "test_common.h"

using namespace pictor;
using namespace pictor_test;

namespace {

/// Six inward-facing planes forming an axis-aligned cube of half-extent `h`.
/// Plane convention: inside ⇔ normal·p + distance >= 0.
Frustum make_cube_frustum(float h) {
    Frustum f;
    f.planes[0] = {{ 1, 0, 0}, h};   // left   x >= -h
    f.planes[1] = {{-1, 0, 0}, h};   // right  x <=  h
    f.planes[2] = {{ 0, 1, 0}, h};   // bottom y >= -h
    f.planes[3] = {{ 0,-1, 0}, h};   // top    y <=  h
    f.planes[4] = {{ 0, 0, 1}, h};   // near   z >= -h
    f.planes[5] = {{ 0, 0,-1}, h};   // far    z <=  h
    return f;
}

AABB box(float cx, float cy, float cz, float r = 0.5f) {
    AABB a;
    a.min = {cx - r, cy - r, cz - r};
    a.max = {cx + r, cy + r, cz + r};
    return a;
}

} // namespace

int main() {
    Frustum f = make_cube_frustum(10.0f);

    // 1. Fully inside.
    PT_ASSERT(f.test_aabb(box(0, 0, 0)), "centre inside");
    PT_ASSERT(f.test_aabb(box(9, 9, 9, 0.5f)), "near corner inside");
    PT_ASSERT(f.test_aabb(box(-9, -9, -9, 0.5f)), "opposite corner inside");

    // 2. Fully outside on each axis.
    PT_ASSERT(!f.test_aabb(box(50, 0, 0)), "far +X outside");
    PT_ASSERT(!f.test_aabb(box(-50, 0, 0)), "far -X outside");
    PT_ASSERT(!f.test_aabb(box(0, 50, 0)), "far +Y outside");
    PT_ASSERT(!f.test_aabb(box(0, 0, 50)), "far +Z outside");
    PT_ASSERT(!f.test_aabb(box(0, 0, -50)), "far -Z outside");

    // 3. Straddling boundary: still considered visible (conservative test).
    PT_ASSERT(f.test_aabb(box(11, 0, 0, 2.0f)),
              "straddling +X plane is visible");
    PT_ASSERT(f.test_aabb(box(-11, 0, 0, 2.0f)),
              "straddling -X plane is visible");

    // 4. Bulk classification: 8x8x8 grid centred on origin.
    {
        const int N = 8;
        const float spacing = 5.0f;
        int visible = 0, hidden = 0;
        for (int z = 0; z < N; ++z)
        for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float cx = (x - N * 0.5f) * spacing + spacing * 0.5f;
            float cy = (y - N * 0.5f) * spacing + spacing * 0.5f;
            float cz = (z - N * 0.5f) * spacing + spacing * 0.5f;
            (f.test_aabb(box(cx, cy, cz, 0.5f)) ? visible : hidden)++;
        }
        // Frustum is half-extent 10, grid spans ±20 → only inner 4^3 = 64 fit.
        PT_ASSERT_OP(visible, ==, 64, "expected visible count");
        PT_ASSERT_OP(hidden, ==, N * N * N - 64, "expected hidden count");
    }

    // 5. Tiny AABB exactly at corner +h is on the boundary → visible.
    PT_ASSERT(f.test_aabb(box(10.0f, 10.0f, 10.0f, 0.01f)),
              "boundary point counts as visible");

    return report("unit_culling_test");
}
