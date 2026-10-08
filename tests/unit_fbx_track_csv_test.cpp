// SPEC-PC-FBX-TRACK-PLAYBACK — track CSV parser and track rotation order.
//
// Headless: compiles demo/fbx_viewer/track_csv.cpp directly. Covers header
// mapping, degree → quaternion order (intrinsic X, Y, Z on top of the bind
// pose), gap / non-numeric rejection and unknown-name tolerance.

#include "test_common.h"

#include "track_csv.h"
#include "track_rotation.h"

#include <cmath>
#include <string>
#include <vector>

using namespace pictor_fbx_viewer;

namespace {

const std::vector<std::string> kBones  = {"Hips", "Neck", "Head"};
const std::vector<std::string> kShapes = {"mouth_open", "blink"};

bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

bool parse(const std::string& csv, TrackData& out, std::string& error) {
    return parse_track_csv(csv, kBones, kShapes, out, error);
}

/// Rotate v by q (v' = q v q*).
pictor::float3 rotate(const pictor::Quaternion& q, pictor::float3 v) {
    const pictor::Quaternion p{v.x, v.y, v.z, 0.0f};
    const pictor::Quaternion r = q * p * q.conjugate();
    return {r.x, r.y, r.z};
}

void test_header_mapping() {
    TrackData t;
    std::string err;
    const bool ok = parse(
        "frame,bone:Head.ry,morph:blink,bone:Head.rx,bone:Neck.rz\n"
        "0,10,0.25,5,-3\n"
        "1,20,0.5,6,-4\r\n"
        "\n",
        t, err);
    PT_ASSERT(ok, "valid track parses");
    PT_ASSERT_OP(t.frame_count, ==, 2u, "two frames");
    PT_ASSERT_OP(t.bone_group_count(), ==, 2u, "Head axes merge into one group, Neck is another");
    PT_ASSERT_OP(t.bone_index[0], ==, 2u, "first group is Head");
    PT_ASSERT_OP(t.bone_index[1], ==, 1u, "second group is Neck");
    PT_ASSERT_OP(t.morph_count(), ==, 1u, "one morph channel");
    PT_ASSERT_OP(t.morph_index[0], ==, 1u, "morph channel targets 'blink'");
    const float* f1 = t.bone_frame(1);
    PT_ASSERT(near(f1[0], 6.0f) && near(f1[1], 20.0f) && near(f1[2], 0.0f), "Head xyz on frame 1, missing rz = 0");
    PT_ASSERT(near(f1[3], 0.0f) && near(f1[4], 0.0f) && near(f1[5], -4.0f), "Neck only rz on frame 1");
    PT_ASSERT(near(t.morph_frame(0)[0], 0.25f) && near(t.morph_frame(1)[0], 0.5f), "morph weights per frame");
    PT_ASSERT(t.warnings.empty(), "no warnings for known names");
}

void test_rejections() {
    TrackData t;
    std::string err;
    PT_ASSERT(!parse("frame,morph:blink\n0,0\n2,1\n", t, err), "frame gap rejected");
    PT_ASSERT(!parse("frame,morph:blink\n1,0\n", t, err), "frame must start at 0");
    PT_ASSERT(!parse("frame,morph:blink\n0,abc\n", t, err), "non-numeric value rejected");
    PT_ASSERT(!parse("frame,morph:blink\n0,\n", t, err), "empty value rejected");
    PT_ASSERT(!parse("frame,morph:blink\n0,1,2\n", t, err), "extra field rejected");
    PT_ASSERT(!parse("frame,morph:blink\n0,1e999\n", t, err), "overflow rejected");
    PT_ASSERT(!parse("time,morph:blink\n0,1\n", t, err), "first column must be frame");
    PT_ASSERT(!parse("frame,bone:Head.rw\n0,1\n", t, err), "bad bone axis rejected");
    PT_ASSERT(!parse("frame,weight:blink\n0,1\n", t, err), "unknown column kind rejected");
    PT_ASSERT(!parse("frame,morph:blink,morph:blink\n0,1,1\n", t, err), "duplicate column rejected");
    PT_ASSERT(!parse("frame,morph:blink\n", t, err), "header without frames rejected");
    PT_ASSERT(!parse("frame,morph:nope,bone:Tail.rx\n0,1,2\n", t, err), "zero known channels is an error");
}

void test_unknown_names_tolerated() {
    TrackData t;
    std::string err;
    const bool ok = parse(
        "frame,bone:Tail.rx,bone:Tail.ry,morph:smile,morph:mouth_open\n"
        "0,1,2,0.3,0.7\n",
        t, err);
    PT_ASSERT(ok, "unknown names do not fail the track");
    PT_ASSERT_OP(t.warnings.size(), ==, size_t{2}, "one warning per unknown name (Tail, smile)");
    PT_ASSERT_OP(t.bone_group_count(), ==, 0u, "unknown bone ignored");
    PT_ASSERT_OP(t.morph_count(), ==, 1u, "known morph kept");
    PT_ASSERT(near(t.morph_frame(0)[0], 0.7f), "known morph value read from the right column");
}

void test_rotation_order() {
    // Intrinsic X then Y: Rx(90) * Ry(90). Rotating +Z: Ry(90) sends +Z to
    // +X, then Rx(90) leaves +X fixed → +X. The extrinsic order would give +Y.
    const pictor::Quaternion q = track_rotation_xyz_deg(90.0f, 90.0f, 0.0f);
    const pictor::float3 z = rotate(q, {0.0f, 0.0f, 1.0f});
    PT_ASSERT(near(z.x, 1.0f, 1e-5f) && near(z.y, 0.0f, 1e-5f) && near(z.z, 0.0f, 1e-5f),
              "Rx * Ry * Rz order (intrinsic X, Y, Z)");

    // Single axis, degrees: 90 deg about Z sends +X to +Y.
    const pictor::float3 x = rotate(track_rotation_xyz_deg(0.0f, 0.0f, 90.0f), {1.0f, 0.0f, 0.0f});
    PT_ASSERT(near(x.x, 0.0f, 1e-5f) && near(x.y, 1.0f, 1e-5f), "degrees about Z");

    // Composed on top of the bind rotation in bone-local space: bind * R.
    pictor::Transform bind;
    bind.translation = {1.0f, 2.0f, 3.0f};
    bind.rotation = pictor::Quaternion::from_axis_angle({0.0f, 1.0f, 0.0f}, 3.14159265f * 0.5f);
    const pictor::Transform t = track_local_transform(bind, 0.0f, 0.0f, 90.0f);
    const pictor::float3 v = rotate(t.rotation, {1.0f, 0.0f, 0.0f});
    // R: +X → +Y; bind (90 deg about Y) leaves +Y fixed.
    PT_ASSERT(near(v.x, 0.0f, 1e-5f) && near(v.y, 1.0f, 1e-5f) && near(v.z, 0.0f, 1e-5f),
              "track rotation applied in bone-local space");
    PT_ASSERT(near(t.translation.x, 1.0f) && near(t.translation.y, 2.0f) && near(t.translation.z, 3.0f),
              "bind translation kept");
    const pictor::Transform id = track_local_transform(bind, 0.0f, 0.0f, 0.0f);
    PT_ASSERT(near(std::fabs(id.rotation.w), std::fabs(bind.rotation.w)) &&
              near(std::fabs(id.rotation.y), std::fabs(bind.rotation.y)), "zero track keeps the bind rotation");
}

} // namespace

int main() {
    test_header_mapping();
    test_rejections();
    test_unknown_names_tolerated();
    test_rotation_order();
    return pictor_test::report("unit_fbx_track_csv_test");
}
