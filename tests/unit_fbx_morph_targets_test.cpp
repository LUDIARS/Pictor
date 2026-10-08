// SPEC-PC-FBX-TRACK-PLAYBACK — CPU blendshape application.
//
// Headless: compiles demo/fbx_viewer/morph_targets.cpp directly. A
// two-vertex fixture must land on base + 0.5 * delta, repeated frames must
// not accumulate, and same-named shapes on two geometries share a channel.

#include "test_common.h"

#include "morph_targets.h"

#include <cmath>
#include <vector>

using namespace pictor_fbx_viewer;

namespace {

bool near(float a, float b) { return std::fabs(a - b) <= 1e-6f; }

// Interleaved like the viewer vertex: xyz followed by other attributes.
constexpr size_t kStride = 5;

void test_half_weight_and_idempotent() {
    std::vector<float> verts = {
        1.0f, 2.0f, 3.0f, 9.0f, 9.0f,
        -1.0f, 0.0f, 4.0f, 9.0f, 9.0f,
    };
    const std::vector<float> deltas = {0.2f, 0.0f, -0.4f, 0.0f, 1.0f, 0.0f};

    MorphTargetsBuilder b;
    b.add_shape("open", 0, 2, deltas.data(), verts.data(), kStride);
    const MorphTargets targets = b.build();
    PT_ASSERT_OP(targets.channel_count(), ==, 1u, "one channel");
    PT_ASSERT_OP(targets.slot_vertex.size(), ==, size_t{2}, "both vertices touched");

    MorphApplier applier;
    applier.bind(&targets);
    const float w = 0.5f;
    for (int frame = 0; frame < 3; ++frame) {
        applier.apply(&w, verts.data(), kStride);
        PT_ASSERT(near(verts[0], 1.1f) && near(verts[1], 2.0f) && near(verts[2], 2.8f), "v0 = base + 0.5 * delta");
        PT_ASSERT(near(verts[5], -1.0f) && near(verts[6], 0.5f) && near(verts[7], 4.0f), "v1 = base + 0.5 * delta");
    }
    PT_ASSERT(near(verts[3], 9.0f) && near(verts[9], 9.0f), "non-position attributes untouched");

    const float zero = 0.0f;
    applier.apply(&zero, verts.data(), kStride);
    PT_ASSERT(near(verts[0], 1.0f) && near(verts[2], 3.0f) && near(verts[6], 0.0f), "weight 0 restores base");
}

void test_sparse_and_shared_channel() {
    // Two geometries (vertex 0 and vertex 1) each carry a shape named
    // "blink"; a third shape touches nothing.
    std::vector<float> verts = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                5.0f, 5.0f, 5.0f, 0.0f, 0.0f};
    const float d_geo0[3] = {1.0f, 0.0f, 0.0f};
    const float d_geo1[3] = {0.0f, 0.0f, 2.0f};
    const float d_none[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    MorphTargetsBuilder b;
    b.add_shape("blink", 0, 1, d_geo0, verts.data(), kStride);
    b.add_shape("empty", 0, 2, d_none, verts.data(), kStride);
    b.add_shape("blink", 1, 1, d_geo1, verts.data(), kStride);
    const MorphTargets targets = b.build();
    PT_ASSERT_OP(targets.channel_count(), ==, 2u, "same-named shapes collapse into one channel");
    PT_ASSERT_OP(targets.entry_slot.size(), ==, size_t{2}, "zero deltas are not stored");

    MorphApplier applier;
    applier.bind(&targets);
    const float weights[2] = {1.0f, 1.0f};
    applier.apply(weights, verts.data(), kStride);
    PT_ASSERT(near(verts[0], 1.0f) && near(verts[7], 7.0f), "one weight drives both geometries");
}

} // namespace

int main() {
    test_half_weight_and_idempotent();
    test_sparse_and_shared_channel();
    return pictor_test::report("unit_fbx_morph_targets_test");
}
