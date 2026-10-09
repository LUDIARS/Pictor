// Frame tap (spec/feature/frame-tap.md, render-tap/1) — 設定解析 / 配送先 / AABB 投影 /
// 安定 ID / 1 行 JSON / 深度順 / OFF 時の無出力 / 流れ (seq・end 行) / 観測者 / 時刻 /
// 名前の付け方 / 世代 / 欠損 / UI の意味 / pass の種類 / 可視性の根拠 /
// PictorRenderer の固定シーン (メッシュ 2 + UI 矩形 1 + 影 pass 1)。
// 契約 id は spec/tasks/2026-10-09-frame-tap-render-tap-1.md の C-1〜C-17。
//
// 固定シーンの出力は作業ディレクトリ (CTest では build ディレクトリ) の
// `frame_tap_fixed_scene.jsonl` にも書く。 Commentarii の schema での検証は
// tools/frame_tap/validate-render-tap.mjs (spec/feature/frame-tap.md §9)。

#include "pictor/core/pictor_renderer.h"
#include "pictor/core/transform_math.h"
#include "pictor/tap/frame_tap.h"
#include "pictor/tap/frame_tap_asset_ids.h"
#include "pictor/tap/frame_tap_clock.h"
#include "pictor/tap/frame_tap_config.h"
#include "pictor/tap/frame_tap_generations.h"
#include "pictor/tap/frame_tap_json_line.h"
#include "pictor/tap/frame_tap_name_table.h"
#include "pictor/tap/frame_tap_projection.h"
#include "pictor/tap/scene_frame_tap_collector.h"
#include "test_common.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace pictor;
using namespace pictor_test;

namespace {

bool approx(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

bool contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

size_t count_of(const std::string& s, const std::string& part) {
    size_t n = 0;
    for (size_t pos = s.find(part); pos != std::string::npos; pos = s.find(part, pos + 1)) ++n;
    return n;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

/// `key` を含む draw オブジェクト 1 つ分 ({ から対応する } まで、 ネストを数える)。
std::string draw_containing(const std::string& line, const std::string& key) {
    const size_t at = line.find(key);
    if (at == std::string::npos) return {};
    const size_t open = line.rfind("{\"mesh\":", at);
    if (open == std::string::npos) return {};
    int depth = 0;
    for (size_t i = open; i < line.size(); ++i) {
        if (line[i] == '{') ++depth;
        if (line[i] == '}' && --depth == 0) return line.substr(open, i - open + 1);
    }
    return {};
}

/// 行を集めるタップ (CallbackFrameTapSink)。
std::unique_ptr<IFrameTapSink> collecting_sink(std::vector<std::string>& lines) {
    return std::make_unique<CallbackFrameTapSink>(
        [&lines](std::string_view line) { lines.emplace_back(line); });
}

AABB box(float x0, float y0, float z0, float x1, float y1, float z1) {
    AABB b;
    b.min = {x0, y0, z0};
    b.max = {x1, y1, z1};
    return b;
}

FrameTapCamera small_camera() {
    FrameTapCamera camera;
    camera.viewport_width  = 100;
    camera.viewport_height = 100;
    return camera;
}

FrameTapSceneObject object_at(MeshHandle mesh, ObjectId id, float depth) {
    FrameTapSceneObject object;
    object.mesh         = mesh;
    object.object       = id;
    object.world_bounds = box(-0.1f, -0.1f, -depth, 0.1f, 0.1f, -depth);
    return object;
}

// ---- C-1 -----------------------------------------------------------------

void test_parse_setting() {
    PT_ASSERT(parse_frame_tap_setting("").output == FrameTapOutput::NONE, "empty is off");
    PT_ASSERT(parse_frame_tap_setting("0").output == FrameTapOutput::NONE, "0 is off");
    PT_ASSERT(parse_frame_tap_setting("OFF").output == FrameTapOutput::NONE, "off ignores case");
    PT_ASSERT(parse_frame_tap_setting("stdout").output == FrameTapOutput::STDOUT, "stdout");
    PT_ASSERT(parse_frame_tap_setting("StdOut").output == FrameTapOutput::STDOUT, "stdout ignores case");
    PT_ASSERT(parse_frame_tap_setting("-").output == FrameTapOutput::STDOUT, "- is stdout");
    const FrameTapSetting file = parse_frame_tap_setting("out/frames.jsonl");
    PT_ASSERT(file.output == FrameTapOutput::FILE, "other value is a file path");
    PT_ASSERT(file.path == "out/frames.jsonl", "file path kept verbatim");
}

// ---- C-2 -----------------------------------------------------------------

void test_sink_creation() {
    std::string error;
    PT_ASSERT(make_frame_tap_sink(FrameTapSetting{}, &error) == nullptr, "NONE has no sink");
    PT_ASSERT(error.empty(), "NONE is not an error");

    FrameTapSetting bad;
    bad.output = FrameTapOutput::FILE;
    bad.path   = (std::filesystem::temp_directory_path() /
                  "pictor_frame_tap_missing_dir" / "x" / "frames.jsonl").string();
    PT_ASSERT(make_frame_tap_sink(bad, &error) == nullptr, "unopenable file yields no sink");
    PT_ASSERT(!error.empty(), "unopenable file reports why");

    // ファイル追記は LF 固定・追記 (既存内容を消さない)。
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "pictor_frame_tap_test.jsonl";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    for (int round = 0; round < 2; ++round) {
        FrameTapSetting good;
        good.output = FrameTapOutput::FILE;
        good.path   = path.string();
        std::unique_ptr<IFrameTapSink> sink = make_frame_tap_sink(good, &error);
        PT_ASSERT(sink != nullptr, "writable file yields a sink");
        if (sink) sink->write_line(round == 0 ? "{\"a\":1}" : "{\"a\":2}");
    }
    std::ifstream in(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    PT_ASSERT(bytes == "{\"a\":1}\n{\"a\":2}\n", "file sink appends LF-terminated lines");
    std::filesystem::remove(path, ec);
}

// ---- C-3 -----------------------------------------------------------------

void test_projection() {
    // クリップ = 座標そのまま (w = 1)。 [-0.5,0.5]^2 → 100x100 の中央 50x50。
    FrameTapScreenRect rect;
    bool on = project_world_aabb_to_screen(box(-0.5f, -0.5f, 0.0f, 0.5f, 0.5f, 0.0f),
                                           float4x4::identity(), 100, 100, rect);
    PT_ASSERT(on, "box inside the view is on screen");
    PT_ASSERT(approx(rect.x, 25.0f) && approx(rect.y, 25.0f), "bbox origin is top-left px");
    PT_ASSERT(approx(rect.w, 50.0f) && approx(rect.h, 50.0f), "bbox size from 8 corners");

    float4x4 proj;
    PT_ASSERT(transform_math::perspective_asymmetric(-1.0f, 1.0f, 1.0f, -1.0f, 0.1f, 100.0f, proj),
              "perspective builds");

    // 視点面をまたぐ箱: 視点の後ろへ広がる辺を w = ε で切ると画面全体へ広がる。
    on = project_world_aabb_to_screen(box(-1.0f, -1.0f, -5.0f, 1.0f, 1.0f, 5.0f),
                                      proj, 200, 100, rect);
    PT_ASSERT(on, "box straddling the eye is on screen");
    PT_ASSERT(approx(rect.x, 0.0f) && approx(rect.y, 0.0f) && approx(rect.w, 200.0f) &&
              approx(rect.h, 100.0f), "straddling box clipped at near-w covers the viewport");

    // 完全に視点の後ろ。
    on = project_world_aabb_to_screen(box(-1.0f, -1.0f, 1.0f, 1.0f, 1.0f, 3.0f),
                                      proj, 200, 100, rect);
    PT_ASSERT(!on, "box behind the eye is off screen");
    PT_ASSERT(rect.w == 0.0f && rect.h == 0.0f, "off-screen rect is zero");

    // 視点深度は AABB 中心の -z_view。
    PT_ASSERT(approx(frame_tap_view_depth(box(-1, -1, -11, 1, 1, -9), float4x4::identity()), 10.0f),
              "view depth is -z of the box center");
}

// ---- C-4 (名前 / 内容ハッシュ / 指紋) ---------------------------------------

void test_asset_ids() {
    std::string id;
    FrameTapMeshInfo named;
    named.name = "crate";
    PT_ASSERT(resolve_mesh_id(named, id) == FrameTapIdentity::ASSET_NAME, "named mesh is asset-name");
    PT_ASSERT(id == "crate", "mesh id is the asset name");

    FrameTapMeshInfo unnamed;
    unnamed.vertex_count = 3;
    unnamed.index_count  = 6;
    PT_ASSERT(resolve_mesh_id(unnamed, id) == FrameTapIdentity::COUNT_HASH,
              "counts-only mesh is count-hash");
    PT_ASSERT(id == "fp:07e2b7092dc71880", "fingerprint is FNV-1a 64 of vertex+index counts");
    PT_ASSERT(frame_tap_mesh_fingerprint(3, 6) != frame_tap_mesh_fingerprint(6, 3),
              "fingerprint depends on the order of counts");

    const float    vertices_a[] = {0.0f, 1.0f, 2.0f};
    const float    vertices_b[] = {0.0f, 1.0f, 3.0f};
    const uint32_t indices[]    = {0, 1, 2};
    uint64_t hash_a = 0, hash_a2 = 0, hash_b = 0, hash_shift = 0;
    PT_ASSERT(frame_tap_mesh_content_hash(vertices_a, sizeof(vertices_a), indices, sizeof(indices), hash_a),
              "content hash from bytes");
    frame_tap_mesh_content_hash(vertices_a, sizeof(vertices_a), indices, sizeof(indices), hash_a2);
    frame_tap_mesh_content_hash(vertices_b, sizeof(vertices_b), indices, sizeof(indices), hash_b);
    // 同じバイト列でも頂点 / インデックスの境目が違えば別の内容。
    frame_tap_mesh_content_hash(vertices_a, sizeof(vertices_a) - 4,
                                reinterpret_cast<const uint8_t*>(vertices_a) + 8, 4 + sizeof(indices),
                                hash_shift);
    PT_ASSERT(hash_a == hash_a2, "content hash is deterministic");
    PT_ASSERT(hash_a != hash_b, "content hash depends on the bytes");
    PT_ASSERT(hash_a != hash_shift, "content hash separates vertex and index bytes");
    uint64_t none = 0;
    PT_ASSERT(!frame_tap_mesh_content_hash(nullptr, 0, nullptr, 0, none), "no data has no content hash");

    FrameTapMeshInfo hashed = unnamed;
    hashed.has_content_hash = true;
    hashed.content_hash     = 0x0123456789abcdefULL;
    PT_ASSERT(resolve_mesh_id(hashed, id) == FrameTapIdentity::CONTENT_HASH,
              "content hash is preferred over counts");
    PT_ASSERT(id == "ch:0123456789abcdef", "content hash id");

    PT_ASSERT(std::string(frame_tap_ui_mesh_id(FrameTapUiKind::NINE_SLICE)) == "ui:nine_slice",
              "ui kind id");
}

// ---- C-5 -----------------------------------------------------------------

void test_json_line_encoding() {
    FrameTapFrame frame;
    frame.seq   = 4;
    frame.frame = 7;
    frame.t     = 0.5;
    frame.has_tick = true;
    frame.tick     = 30;
    frame.camera.viewport_width  = 4;
    frame.camera.viewport_height = 2;
    frame.passes.resize(2);
    frame.passes[0] = {"scene", FrameTapPassKind::SCENE};
    frame.passes[1] = {"ui", FrameTapPassKind::UI};
    frame.pass_count = 2;
    FrameTapDraw draw;
    draw.pass        = kFrameTapScenePass;
    draw.mesh        = "a\"b";
    draw.materials   = {"m"};
    draw.instance    = 3;
    draw.screen_bbox = {1.0f, 2.0f, 3.0f, 4.0f};
    draw.visibility  = FrameTapVisibility::FRUSTUM_ONLY;
    draw.tags        = {"unnamed"};
    frame.draws.push_back(draw);
    frame.draw_count = 1;

    std::string line;
    encode_frame_tap_line(frame, line);
    const std::string I = "[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]";
    const std::string expected =
        "{\"contract\":\"render-tap/1\",\"seq\":4,\"frame\":7,\"tick\":30,\"t\":0.5,"
        "\"observer\":{\"id\":\"player-camera\",\"viewport\":[4,2]},"
        "\"camera\":{\"view\":" + I + ",\"projection\":" + I + "},"
        "\"passes\":[{\"name\":\"scene\",\"kind\":\"scene\",\"draws\":[{\"mesh\":\"a\\\"b\","
        "\"material\":[\"m\"],\"identity\":\"asset-name\",\"instance\":3,\"generation\":0,"
        "\"world\":" + I + ",\"screen_bbox\":[1,2,3,4],\"depth_order\":0,"
        "\"visibility\":\"frustum-only\",\"tags\":[\"unnamed\"]}]},"
        "{\"name\":\"ui\",\"kind\":\"ui\",\"draws\":[]}]}";
    PT_ASSERT(line == expected, "frame encodes to the render-tap/1 line");
    if (line != expected) std::fprintf(stderr, "       got: %s\n", line.c_str());
    PT_ASSERT(line.find('\n') == std::string::npos, "line has no newline");

    // 任意の項目: visibility_lag_frames / dropped / alpha / clip / ui。
    frame.has_visibility_lag    = true;
    frame.visibility_lag_frames = 1;
    frame.dropped_draws  = 2;
    frame.dropped_reason = FrameTapDropReason::BACKPRESSURE;
    frame.draws[0].has_alpha = true;
    frame.draws[0].alpha     = 0.25f;
    frame.draws[0].has_clip  = true;
    frame.draws[0].clip      = {0.0f, 0.0f, 10.0f, 5.0f};
    frame.draws[0].ui_role    = FrameTapUiRole::BAR;
    frame.draws[0].ui_element = "hud.hp";
    frame.draws[0].ui_fill    = 0.5f;
    frame.draws.push_back(draw);  // draw_count 外の slot は出さない
    encode_frame_tap_line(frame, line);
    PT_ASSERT(contains(line, "},\"visibility_lag_frames\":1,\"passes\":"), "lag follows camera");
    PT_ASSERT(contains(line, "\"alpha\":0.25,\"clip\":[0,0,10,5],"
                             "\"ui\":{\"role\":\"bar\",\"element\":\"hud.hp\",\"fill\":0.5}"),
              "alpha, clip and bar ui");
    PT_ASSERT(contains(line, "]}],\"dropped\":{\"draws\":2,\"reason\":\"backpressure\"}}"),
              "dropped closes the frame line");
    PT_ASSERT(count_of(line, "\"mesh\"") == 1, "only draw_count draws are emitted");

    frame.draws[0].ui_role  = FrameTapUiRole::GLYPH;
    frame.draws[0].ui_glyph = "87";
    frame.draws[0].ui_element.clear();
    encode_frame_tap_line(frame, line);
    PT_ASSERT(contains(line, "\"ui\":{\"role\":\"glyph\",\"glyph\":\"87\"}"), "glyph ui without element");

    encode_frame_tap_end_line(9, FrameTapEndReason::FAILED, line);
    PT_ASSERT(line == "{\"contract\":\"render-tap/1\",\"seq\":9,\"end\":\"error\"}", "end line");
}

// ---- C-6 / C-7 -------------------------------------------------------------

void test_depth_order_and_lines() {
    std::vector<std::string> lines;
    FrameTapNameTable names;
    names.set_mesh(0, "far", 3, 3);
    names.set_mesh(1, "near", 3, 3);
    names.set_ui_texture_name(5, "hp_bar");

    FrameTap tap;
    tap.set_asset_source(&names);
    tap.set_sink(collecting_sink(lines));
    tap.begin_frame(1, 0.0, small_camera());
    tap.add_scene_draw(object_at(0, 10, 20.0f));
    tap.add_scene_draw(object_at(1, 11, 2.0f));

    FrameTapUiDraw back;
    back.instance = 0;
    FrameTapUiDraw front = back;
    front.kind       = FrameTapUiKind::IMAGE;
    front.texture_id = 5;
    front.instance   = 1;
    tap.add_ui_draw(back);
    tap.add_ui_draw(front);
    tap.end_frame();

    PT_ASSERT_OP(lines.size(), ==, size_t{1}, "one line per frame");
    if (lines.size() == 1) {
        const std::string& l = lines[0];
        PT_ASSERT(contains(draw_containing(l, "\"mesh\":\"far\""), "\"depth_order\":1"),
                  "far scene draw is behind");
        PT_ASSERT(contains(draw_containing(l, "\"mesh\":\"near\""), "\"depth_order\":0"),
                  "near scene draw is front");
        // ui は契約どおり提出順 (0 = 最初に描いた)。
        PT_ASSERT(contains(draw_containing(l, "\"mesh\":\"ui:rect\""), "\"depth_order\":0"),
                  "first ui draw has depth_order 0");
        PT_ASSERT(contains(draw_containing(l, "\"mesh\":\"ui:image\""), "\"depth_order\":1"),
                  "later ui draw has depth_order 1");
        PT_ASSERT(l.find("\"mesh\":\"near\"") < l.find("\"name\":\"ui\""), "scene pass precedes ui");
        PT_ASSERT(contains(l, "\"mesh\":\"ui:image\",\"material\":[\"hp_bar\"],"
                              "\"identity\":\"asset-name\",\"instance\":4294967297,"),
                  "ui texture name is the material id and ui instance is offset");
    }
    PT_ASSERT_OP(tap.frames_written(), ==, uint64_t{1}, "frames_written counts lines");

    // フレーム外の draw は捨てて数える。
    tap.add_ui_draw(back);
    PT_ASSERT_OP(tap.dropped_draws(), ==, uint64_t{1}, "draw outside a frame is dropped");

    // C-7: 無効化すると end 行で閉じ、 その後は何も出さない。
    lines.clear();
    tap.disable();
    PT_ASSERT(!tap.is_enabled(), "disabled tap reports off");
    PT_ASSERT(lines.size() == 1 && contains(lines[0], "\"end\":\"shutdown\""),
              "disable closes the stream with an end line");
    lines.clear();
    tap.begin_frame(3, 0.2, small_camera());
    tap.add_scene_draw(object_at(1, 11, 2.0f));
    tap.end_frame();
    tap.disable();
    PT_ASSERT(lines.empty(), "disabled tap writes nothing");
    PT_ASSERT_OP(tap.dropped_draws(), ==, uint64_t{1}, "disabled tap does not count drops");
}

// ---- C-8: 流れ (seq / end 行) ------------------------------------------------

void test_stream_sequence() {
    std::vector<std::string> lines;
    FrameTap tap;
    tap.set_sink(collecting_sink(lines));

    tap.begin_frame(10, 0.0, small_camera());
    tap.end_frame();                              // seq 0
    tap.begin_frame(11, 0.0, small_camera());     // seq 1 — 閉じずに次を開く = 捨てる
    tap.begin_frame(12, 0.0, small_camera());
    tap.end_frame();                              // seq 2
    FrameTapCamera broken = small_camera();
    broken.viewport_width = 0;
    tap.begin_frame(13, 0.0, broken);
    tap.end_frame();                              // seq 3 — viewport 0 は出せない
    tap.begin_frame(14, 0.0, small_camera());
    tap.end_frame();                              // seq 4
    tap.end_stream(FrameTapEndReason::FAILED);    // seq 5

    PT_ASSERT_OP(lines.size(), ==, size_t{4}, "3 frame lines + end");
    if (lines.size() == 4) {
        PT_ASSERT(starts_with(lines[0], "{\"contract\":\"render-tap/1\",\"seq\":0,\"frame\":10,"), "seq 0");
        PT_ASSERT(starts_with(lines[1], "{\"contract\":\"render-tap/1\",\"seq\":2,\"frame\":12,"),
                  "discarded frame leaves a seq gap");
        PT_ASSERT(starts_with(lines[2], "{\"contract\":\"render-tap/1\",\"seq\":4,\"frame\":14,"),
                  "unemittable frame leaves a seq gap");
        PT_ASSERT(lines[3] == "{\"contract\":\"render-tap/1\",\"seq\":5,\"end\":\"error\"}",
                  "end_stream writes the error end line");
    }
    PT_ASSERT_OP(tap.dropped_frames(), ==, uint64_t{2}, "two frames dropped");
    PT_ASSERT(!tap.is_enabled(), "end_stream disables the tap");

    // 配送先の差し替え: 前の流れを end で閉じ、 新しい流れは seq 0 から。
    std::vector<std::string> first, second;
    tap.set_sink(collecting_sink(first));
    tap.begin_frame(20, 0.0, small_camera());
    tap.set_sink(collecting_sink(second));
    tap.begin_frame(21, 0.0, small_camera());
    tap.end_frame();
    tap.disable();
    PT_ASSERT(first.size() == 1 && first[0] == "{\"contract\":\"render-tap/1\",\"seq\":1,\"end\":\"shutdown\"}",
              "replacing the sink ends the old stream after the discarded frame");
    PT_ASSERT(second.size() == 2 && starts_with(second[0], "{\"contract\":\"render-tap/1\",\"seq\":0,") &&
              second[1] == "{\"contract\":\"render-tap/1\",\"seq\":1,\"end\":\"shutdown\"}",
              "new stream starts at seq 0 and ends with shutdown");

    // 破棄でも end 行を出す。
    std::vector<std::string> scoped;
    {
        FrameTap local;
        local.set_sink(collecting_sink(scoped));
    }
    PT_ASSERT(scoped.size() == 1 && contains(scoped[0], "\"end\":\"shutdown\""),
              "destroying an enabled tap writes the end line");
}

// ---- C-9: 観測者 ------------------------------------------------------------

void test_observer() {
    std::vector<std::string> lines;
    FrameTap tap;
    PT_ASSERT(tap.observer_id() == "player-camera", "default observer is the player camera");
    PT_ASSERT(!tap.set_observer_id("Player Camera"), "invalid observer id rejected");
    PT_ASSERT(!tap.set_observer_id(""), "empty observer id rejected");
    PT_ASSERT(!tap.set_observer_id("-cam"), "observer id cannot start with a symbol");
    PT_ASSERT(tap.observer_id() == "player-camera", "rejected id keeps the previous one");
    PT_ASSERT(tap.set_observer_id("mirror.cam_2"), "valid observer id accepted");

    tap.set_sink(collecting_sink(lines));
    tap.begin_frame(1, 0.0, small_camera());
    tap.end_frame();
    PT_ASSERT(lines.size() == 1 &&
              contains(lines[0], "\"observer\":{\"id\":\"mirror.cam_2\",\"viewport\":[100,100]}"),
              "observer id and viewport");
    tap.disable();
}

// ---- C-10: 時刻 -------------------------------------------------------------

void test_clock() {
    FrameTapClock clock;
    uint64_t tick = 0;
    PT_ASSERT(clock.next_time(0.5) == 0.5, "fallback time used without host clock");
    PT_ASSERT(!clock.next_tick(tick), "no tick without host tick");
    PT_ASSERT(clock.next_time(0.25) == 0.5, "fallback time never decreases");

    clock.set_game_time(4.0);
    clock.set_game_tick(40);
    PT_ASSERT(clock.next_time(99.0) == 4.0, "host game time wins over fallback");
    PT_ASSERT(clock.next_tick(tick) && tick == 40, "host tick");
    clock.set_game_time(3.0);
    clock.set_game_tick(39);
    PT_ASSERT(clock.next_time(0.0) == 4.0, "host time going back is held");
    PT_ASSERT(clock.next_tick(tick) && tick == 40, "host tick going back is held");
    clock.set_game_time(std::numeric_limits<double>::quiet_NaN());
    PT_ASSERT(clock.next_time(0.0) == 4.0, "non-finite host time is held");

    clock.clear_game_clock();
    PT_ASSERT(!clock.next_tick(tick), "cleared clock stops ticks");
    clock.reset_stream();
    PT_ASSERT(clock.next_time(-1.0) == 0.0, "negative time clamps to 0 on a new stream");

    // タップ経由: 行の t と tick。
    std::vector<std::string> lines;
    FrameTap tap;
    tap.set_sink(collecting_sink(lines));
    tap.clock().set_game_time(4.0);
    tap.clock().set_game_tick(40);
    tap.begin_frame(100, 0.0, small_camera());
    tap.end_frame();
    PT_ASSERT(lines.size() == 1 && contains(lines[0], "\"frame\":100,\"tick\":40,\"t\":4,"),
              "line carries host tick and game time");
    tap.disable();
}

// ---- C-11: 名前の付け方 (identity) --------------------------------------------

void test_identity() {
    std::vector<std::string> lines;
    FrameTapNameTable names;
    names.set_mesh(0, "crate", 24, 36);
    names.set_mesh(1, "", 24, 36);
    names.set_mesh_content_hash(1, 0xfedcba9876543210ULL);
    names.set_mesh(2, "", 3, 6);
    names.set_material_name(0, "wood");

    FrameTap tap;
    tap.set_asset_source(&names);
    tap.set_sink(collecting_sink(lines));
    tap.begin_frame(1, 0.0, small_camera());
    FrameTapSceneObject named = object_at(0, 1, 5.0f);
    named.material = 0;
    FrameTapSceneObject hashed = object_at(1, 2, 5.0f);
    FrameTapSceneObject counted = object_at(2, 3, 5.0f);
    FrameTapSceneObject unnamed_material = object_at(0, 4, 5.0f);
    unnamed_material.material = 7;
    tap.add_scene_draw(named);
    tap.add_scene_draw(hashed);
    tap.add_scene_draw(counted);
    tap.add_scene_draw(unnamed_material);
    tap.end_frame();
    tap.disable();

    PT_ASSERT(!lines.empty(), "frame emitted");
    if (lines.empty()) return;
    const std::string& l = lines[0];
    PT_ASSERT(contains(l, "\"mesh\":\"crate\",\"material\":[\"wood\"],\"identity\":\"asset-name\",\"instance\":1,"),
              "named mesh + material is asset-name");
    PT_ASSERT(contains(l, "\"mesh\":\"ch:fedcba9876543210\",\"material\":[],\"identity\":\"content-hash\",\"instance\":2,"),
              "unnamed mesh with content hash is content-hash");
    PT_ASSERT(contains(l, "\"mesh\":\"fp:07e2b7092dc71880\",\"material\":[],\"identity\":\"count-hash\",\"instance\":3,"),
              "counts-only mesh is count-hash");
    const std::string downgraded = draw_containing(l, "\"instance\":4,");
    PT_ASSERT(contains(downgraded, "\"mesh\":\"crate\",\"material\":[],\"identity\":\"count-hash\"") &&
              contains(downgraded, "\"unnamed\""),
              "unnamed material hides the handle and is not identifiable");
    PT_ASSERT(contains(draw_containing(l, "\"instance\":2,"), "\"tags\":[\"unnamed\"]"),
              "content-hash draw is still tagged unnamed (no asset name)");
}

// ---- C-12: 世代 -------------------------------------------------------------

void test_generations() {
    FrameTapGenerations generations;
    uint32_t g = 99;
    PT_ASSERT(generations.observe(FrameTapInstanceSpace::SCENE, 3, 111, g) && g == 0, "first sight is 0");
    PT_ASSERT(generations.observe(FrameTapInstanceSpace::SCENE, 3, 111, g) && g == 0, "same look keeps 0");
    PT_ASSERT(generations.observe(FrameTapInstanceSpace::SCENE, 3, 222, g) && g == 1, "new look bumps");
    PT_ASSERT(generations.observe(FrameTapInstanceSpace::UI, 3, 999, g) && g == 0, "ui space is separate");
    PT_ASSERT(!generations.observe(FrameTapInstanceSpace::UI, FrameTapGenerations::kMaxTrackedInstances, 1, g),
              "untrackable instance is reported");
    generations.reset();
    PT_ASSERT(generations.observe(FrameTapInstanceSpace::SCENE, 3, 222, g) && g == 0, "reset forgets");

    std::vector<uint64_t> scratch;
    PT_ASSERT(frame_tap_appearance_hash("m", {"a", "b"}, scratch) ==
              frame_tap_appearance_hash("m", {"b", "a"}, scratch), "appearance ignores material order");
    PT_ASSERT(frame_tap_appearance_hash("m", {"a"}, scratch) !=
              frame_tap_appearance_hash("n", {"a"}, scratch), "appearance depends on the mesh");

    // UI の添字は毎フレーム別の物を指しうる: 外見が変われば世代が上がる。
    std::vector<std::string> lines;
    FrameTapNameTable names;
    names.set_ui_texture_name(5, "icon");
    FrameTap tap;
    tap.set_asset_source(&names);
    tap.set_sink(collecting_sink(lines));
    FrameTapUiDraw rect;
    tap.begin_frame(1, 0.0, small_camera());
    tap.add_ui_draw(rect);
    tap.end_frame();
    FrameTapUiDraw image = rect;
    image.kind       = FrameTapUiKind::IMAGE;
    image.texture_id = 5;
    tap.begin_frame(2, 0.0, small_camera());
    tap.add_ui_draw(image);
    tap.end_frame();
    tap.disable();
    PT_ASSERT(lines.size() == 3 &&
              contains(lines[0], "\"instance\":4294967296,\"generation\":0,") &&
              contains(lines[1], "\"instance\":4294967296,\"generation\":1,"),
              "reused ui instance with another look gets a new generation");
}

// ---- C-13: 欠損 (dropped) ----------------------------------------------------

void test_dropped() {
    std::vector<std::string> lines;
    FrameTap tap;
    tap.set_sink(collecting_sink(lines));

    tap.begin_frame(1, 0.0, small_camera());
    FrameTapSceneObject good = object_at(0, 1, 5.0f);
    FrameTapSceneObject nan_world = good;
    nan_world.object = 2;
    nan_world.world.m[3][0] = std::numeric_limits<float>::quiet_NaN();
    FrameTapSceneObject inf_bounds = good;
    inf_bounds.object = 3;
    inf_bounds.world_bounds.max.x = std::numeric_limits<float>::infinity();
    FrameTapSceneObject bad_pass = good;
    bad_pass.object = 4;
    bad_pass.pass   = 7;
    FrameTapUiDraw bad_fill;
    bad_fill.role = FrameTapUiRole::BAR;
    bad_fill.fill = 1.5f;
    FrameTapUiDraw negative_size;
    negative_size.w = -1.0f;
    tap.add_scene_draw(good);
    tap.add_scene_draw(nan_world);
    tap.add_scene_draw(inf_bounds);
    tap.add_scene_draw(bad_pass);
    tap.add_ui_draw(bad_fill);
    tap.add_ui_draw(negative_size);
    tap.end_frame();

    PT_ASSERT(lines.size() == 1, "frame emitted");
    if (lines.size() == 1) {
        const std::string& l = lines[0];
        PT_ASSERT(!contains(l, "null"), "no null in the line");
        PT_ASSERT_OP(count_of(l, "\"mesh\":"), ==, size_t{1}, "only the finite draw is emitted");
        PT_ASSERT(contains(l, "\"dropped\":{\"draws\":5,\"reason\":\"other\"}"),
                  "invalid draws are counted as dropped (other)");
    }

    // 上限: 超えた分は buffer-full。
    lines.clear();
    tap.set_max_draws_per_frame(1);
    tap.begin_frame(2, 0.0, small_camera());
    tap.add_scene_draw(good);
    tap.add_scene_draw(good);
    tap.add_scene_draw(nan_world);
    tap.end_frame();
    PT_ASSERT(lines.size() == 1 && contains(lines[0], "\"dropped\":{\"draws\":2,\"reason\":\"buffer-full\"}"),
              "draws over the cap are buffer-full");
    lines.clear();
    tap.begin_frame(3, 0.0, small_camera());
    tap.end_frame();
    PT_ASSERT(lines.size() == 1 && !contains(lines[0], "dropped"), "complete frame has no dropped");
    tap.disable();
}

// ---- C-14: UI の意味 (clip / ui / alpha) -------------------------------------

void test_ui_semantics() {
    std::vector<std::string> lines;
    FrameTap tap;
    tap.set_sink(collecting_sink(lines));
    tap.begin_frame(1, 0.0, small_camera());

    FrameTapUiDraw bar;
    bar.x = 20; bar.y = 20; bar.w = 200; bar.h = 16;
    bar.instance = 900;
    bar.alpha    = 1.5f;  // 色 × opacity が 1 を超えても最終不透明度は 1
    bar.role     = FrameTapUiRole::BAR;
    bar.element  = "hud.hp";
    bar.fill     = 0.8f;
    FrameTapUiDraw digit;
    digit.x = 90; digit.y = 60; digit.w = 40; digit.h = 18;
    digit.instance = 903;
    digit.alpha    = 0.5f;
    digit.has_clip = true;
    digit.clip     = {0, 0, 80, 100};
    digit.role     = FrameTapUiRole::GLYPH;
    digit.element  = "enemy-hp-number";
    digit.glyph    = "87";
    FrameTapUiDraw empty_glyph = digit;
    empty_glyph.instance = 904;
    empty_glyph.glyph    = "";
    tap.add_ui_draw(bar);
    tap.add_ui_draw(digit);
    tap.add_ui_draw(empty_glyph);
    tap.end_frame();
    tap.disable();

    PT_ASSERT(lines.size() == 2, "frame + end");
    if (lines.empty()) return;
    const std::string& l = lines[0];
    PT_ASSERT(contains(draw_containing(l, "\"instance\":4294968196,"),
                       "\"visibility\":\"unknown\",\"alpha\":1,"
                       "\"ui\":{\"role\":\"bar\",\"element\":\"hud.hp\",\"fill\":0.8}"),
              "bar with fill and clamped alpha");
    PT_ASSERT(contains(draw_containing(l, "\"instance\":4294968199,"),
                       "\"alpha\":0.5,\"clip\":[0,0,80,100],"
                       "\"ui\":{\"role\":\"glyph\",\"element\":\"enemy-hp-number\",\"glyph\":\"87\"}"),
              "glyph with clip");
    const std::string empty = draw_containing(l, "\"instance\":4294968200,");
    PT_ASSERT(!empty.empty() && !contains(empty, "\"ui\":"), "empty glyph drops only the ui semantics");
}

// ---- C-15: pass の種類 --------------------------------------------------------

void test_pass_kinds() {
    PT_ASSERT(std::string(frame_tap_pass_kind_name(FrameTapPassKind::DEPTH_PREPASS)) == "depth-prepass",
              "depth-prepass kind name");
    std::vector<std::string> lines;
    FrameTap tap;
    PT_ASSERT(tap.add_pass("shadow", FrameTapPassKind::SHADOW) == FrameTap::kInvalidPass,
              "no pass outside a frame");
    tap.set_sink(collecting_sink(lines));
    tap.begin_frame(1, 0.0, small_camera());
    PT_ASSERT(tap.add_pass("", FrameTapPassKind::SHADOW) == FrameTap::kInvalidPass, "empty name rejected");
    const uint32_t shadow = tap.add_pass("shadow-cascade", FrameTapPassKind::SHADOW);
    const uint32_t mirror = tap.add_pass("mirror", FrameTapPassKind::REFLECTION);
    PT_ASSERT(shadow == 2 && mirror == 3, "added passes follow scene and ui");
    FrameTapSceneObject caster = object_at(0, 1, 5.0f);
    caster.pass = shadow;
    tap.add_scene_draw(caster);
    tap.add_scene_draw(object_at(0, 1, 5.0f));
    tap.end_frame();
    // 次のフレームは scene / ui だけに戻る。
    tap.begin_frame(2, 0.0, small_camera());
    tap.end_frame();
    tap.disable();

    PT_ASSERT(lines.size() == 3, "two frames + end");
    if (lines.size() < 2) return;
    PT_ASSERT(contains(lines[0], "{\"name\":\"scene\",\"kind\":\"scene\",\"draws\":[{") &&
              contains(lines[0], "{\"name\":\"ui\",\"kind\":\"ui\",\"draws\":[]}") &&
              contains(lines[0], "{\"name\":\"shadow-cascade\",\"kind\":\"shadow\",\"draws\":[{") &&
              contains(lines[0], "{\"name\":\"mirror\",\"kind\":\"reflection\",\"draws\":[]}"),
              "passes carry name and kind");
    PT_ASSERT(!contains(lines[1], "shadow"), "added passes last one frame");
}

// ---- C-16: 可視性の根拠 -------------------------------------------------------

class FixedVisibility final : public IFrameTapVisibilitySource {
public:
    FrameTapVisibility visibility(ObjectId object) const override {
        return object == 2 ? FrameTapVisibility::OCCLUSION_FAILED : FrameTapVisibility::OCCLUSION_PASSED;
    }
    uint32_t lag_frames() const override { return 1; }
};

void test_visibility_evidence() {
    std::vector<std::string> lines;
    FrameTap tap;
    tap.set_sink(collecting_sink(lines));
    tap.begin_frame(1, 0.0, small_camera());
    tap.add_scene_draw(object_at(0, 1, 5.0f));
    tap.end_frame();
    PT_ASSERT(lines.size() == 1 && contains(lines[0], "\"visibility\":\"frustum-only\"") &&
              !contains(lines[0], "visibility_lag_frames"),
              "without occlusion evidence: frustum-only and no lag");

    FixedVisibility source;
    tap.set_visibility_source(&source);
    MemoryConfig    memory_config;
    MemorySubsystem memory(memory_config);
    SceneRegistry   scene(memory);
    ObjectDescriptor a;
    a.mesh   = 0;
    a.bounds = box(-1, -1, -6, 1, 1, -4);
    ObjectDescriptor b = a;
    scene.register_object(a);
    scene.register_object(b);
    scene.register_object(b);
    for (ObjectPool* pool : {&scene.static_pool(), &scene.dynamic_pool()}) {
        for (uint32_t i = 0; i < pool->count(); ++i) pool->visibility_flags().data()[i] = 1;
    }
    lines.clear();
    tap.begin_frame(2, 0.0, small_camera());
    collect_scene_frame_tap(scene, tap);
    tap.end_frame();
    tap.disable();
    PT_ASSERT(lines.size() == 2, "frame + end");
    if (lines.empty()) return;
    PT_ASSERT(contains(lines[0], "\"visibility_lag_frames\":1"), "lag from the evidence source");
    PT_ASSERT(contains(draw_containing(lines[0], "\"instance\":2,"), "\"visibility\":\"occlusion-failed\""),
              "collector asks the evidence source");
    PT_ASSERT(contains(draw_containing(lines[0], "\"instance\":0,"), "\"visibility\":\"occlusion-passed\""),
              "passed occlusion");
}

// ---- C-17: 固定シーン (メッシュ 2 + UI 矩形 1 + 影 pass 1) ------------------------

struct SceneRun {
    std::vector<std::string> lines;
    uint32_t visible = 0;
    uint32_t batches = 0;
};

SceneRun run_fixed_scene(bool tap_on) {
    SceneRun run;
    RendererConfig cfg;
    cfg.initial_profile  = "Standard";
    cfg.profiler_enabled = true;
    cfg.overlay_mode     = OverlayMode::OFF;
    cfg.screen_width     = 1280;
    cfg.screen_height    = 720;
    cfg.frame_tap        = "off";  // 環境変数に左右されないよう明示し、 API で付ける

    PictorRenderer renderer;
    renderer.initialize(cfg);
    if (tap_on) {
        renderer.frame_tap().set_sink(collecting_sink(run.lines));
        renderer.frame_tap().clock().set_game_tick(40);
    }

    MeshDataDescriptor crate_mesh;
    crate_mesh.name         = "crate";
    crate_mesh.vertex_count = 24;
    crate_mesh.index_count  = 36;
    MeshDataDescriptor barrel_mesh;
    barrel_mesh.name         = "barrel";
    barrel_mesh.vertex_count = 64;
    barrel_mesh.index_count  = 180;
    const MeshHandle crate  = renderer.register_mesh_data(crate_mesh);
    const MeshHandle barrel = renderer.register_mesh_data(barrel_mesh);
    renderer.frame_tap_names().set_material_name(0, "wood");
    renderer.frame_tap_names().set_material_name(1, "iron");

    ObjectDescriptor a;
    a.mesh     = crate;
    a.material = 0;
    a.materialKey = 0;
    a.transform.set_translation(-2.0f, 0.0f, 0.0f);
    a.bounds   = box(-3.0f, -1.0f, -1.0f, -1.0f, 1.0f, 1.0f);
    ObjectDescriptor b;
    b.mesh     = barrel;
    b.material = 1;
    b.materialKey = 1;
    b.transform.set_translation(2.0f, 0.0f, -5.0f);
    b.bounds   = box(1.0f, -1.0f, -6.0f, 3.0f, 1.0f, -4.0f);
    const ObjectId id_a = renderer.register_object(a);
    const ObjectId id_b = renderer.register_object(b);

    Camera camera;
    camera.view.set_translation(0.0f, 0.0f, -10.0f);  // 視点は z = +10 から -z を見る
    transform_math::perspective_asymmetric(-1.0f, 1.0f, 0.5625f, -0.5625f, 0.1f, 100.0f,
                                           camera.projection);
    for (int i = 0; i < 6; ++i) camera.frustum.planes[i].distance = 1000.0f;
    camera.frustum.planes[0].normal = { 1, 0, 0};
    camera.frustum.planes[1].normal = {-1, 0, 0};
    camera.frustum.planes[2].normal = { 0, 1, 0};
    camera.frustum.planes[3].normal = { 0,-1, 0};
    camera.frustum.planes[4].normal = { 0, 0, 1};
    camera.frustum.planes[5].normal = { 0, 0,-1};

    for (int frame = 0; frame < 2; ++frame) {
        renderer.begin_frame(1.0f / 60.0f);
        renderer.render(camera);
        if (tap_on) {
            // 影 pass はホストが描くので、 ホストが同じタップへ足す (crate が影を落とす)。
            FrameTap& tap = renderer.frame_tap();
            const uint32_t shadow = tap.add_pass("shadow-cascade", FrameTapPassKind::SHADOW);
            FrameTapSceneObject caster;
            caster.mesh         = crate;
            caster.material     = 0;
            caster.object       = id_a;
            caster.world        = a.transform;
            caster.world_bounds = a.bounds;
            caster.pass         = shadow;
            tap.add_scene_draw(caster);
        }
        // UIRenderer::record() がタップへ流すのと同じ入口 (headless なので直接呼ぶ)。
        FrameTapUiDraw hud;
        hud.kind = FrameTapUiKind::RECT;
        hud.x = 16.0f; hud.y = 16.0f; hud.w = 200.0f; hud.h = 24.0f;
        hud.role    = FrameTapUiRole::BAR;
        hud.element = "hud.hp";
        hud.fill    = frame == 0 ? 0.8f : 0.7f;
        renderer.frame_tap().add_ui_draw(hud);
        renderer.end_frame();
    }

    run.visible = renderer.get_frame_stats().visible_objects;
    run.batches = renderer.get_frame_stats().batch_count;
    PT_ASSERT(id_a != id_b, "two distinct objects");
    renderer.shutdown();
    return run;
}

void write_fixed_scene_output(const std::vector<std::string>& lines) {
    std::ofstream out("frame_tap_fixed_scene.jsonl", std::ios::binary | std::ios::trunc);
    for (const std::string& line : lines) out << line << '\n';
}

void test_renderer_fixed_scene() {
    const SceneRun off = run_fixed_scene(false);
    const SceneRun on  = run_fixed_scene(true);

    PT_ASSERT(off.lines.empty(), "tap off emits nothing");
    PT_ASSERT_OP(on.visible, ==, off.visible, "tap does not change visibility");
    PT_ASSERT_OP(on.batches, ==, off.batches, "tap does not change batching");
    PT_ASSERT_OP(on.lines.size(), ==, size_t{3}, "two frame lines + end line");
    if (on.lines.size() != 3) return;
    write_fixed_scene_output(on.lines);

    const std::string& l = on.lines[0];
    PT_ASSERT(starts_with(l, "{\"contract\":\"render-tap/1\",\"seq\":0,\"frame\":1,\"tick\":40,\"t\":0.016"),
              "contract, seq, frame, tick and t lead the line");
    PT_ASSERT(starts_with(on.lines[1], "{\"contract\":\"render-tap/1\",\"seq\":1,\"frame\":2,"),
              "second frame follows");
    PT_ASSERT(on.lines[2] == "{\"contract\":\"render-tap/1\",\"seq\":2,\"end\":\"shutdown\"}",
              "shutdown writes the end line");
    PT_ASSERT(contains(l, "\"observer\":{\"id\":\"player-camera\",\"viewport\":[1280,720]}"), "observer");
    PT_ASSERT(!contains(l, "visibility_lag_frames") && !contains(l, "dropped"), "no lag, nothing dropped");
    PT_ASSERT(contains(l, "\"passes\":[{\"name\":\"scene\",\"kind\":\"scene\",\"draws\":[") &&
              contains(l, "{\"name\":\"ui\",\"kind\":\"ui\",\"draws\":[{\"mesh\":\"ui:rect\",\"material\":[],"
                          "\"identity\":\"asset-name\",\"instance\":4294967296,\"generation\":0,") &&
              contains(l, "{\"name\":\"shadow-cascade\",\"kind\":\"shadow\",\"draws\":[{\"mesh\":\"crate\""),
              "scene, ui and shadow passes");
    PT_ASSERT_OP(count_of(l, "\"mesh\":"), ==, size_t{4}, "2 scene draws + 1 ui draw + 1 shadow draw");
    PT_ASSERT(contains(l, "\"mesh\":\"crate\",\"material\":[\"wood\"],\"identity\":\"asset-name\""), "crate ids");
    PT_ASSERT(contains(l, "\"mesh\":\"barrel\",\"material\":[\"iron\"],\"identity\":\"asset-name\""), "barrel ids");
    PT_ASSERT(!contains(l, "unnamed") && !contains(l, "offscreen") && !contains(l, "null"),
              "no fallback tags and no null");
    PT_ASSERT(contains(l, "\"screen_bbox\":[16,16,200,24],\"depth_order\":0,\"visibility\":\"unknown\","
                          "\"alpha\":1,\"ui\":{\"role\":\"bar\",\"element\":\"hud.hp\",\"fill\":0.8}"),
              "ui rect with bar semantics");
    PT_ASSERT(contains(on.lines[1], "\"fill\":0.7"), "fill follows the host value per frame");

    // crate (深度 10) が barrel (深度 15) より前。 scene の可視性は frustum-only。
    const size_t scene_end = l.find("{\"name\":\"ui\"");
    const std::string scene = l.substr(0, scene_end);
    const std::string crate_draw  = draw_containing(scene, "\"mesh\":\"crate\"");
    const std::string barrel_draw = draw_containing(scene, "\"mesh\":\"barrel\"");
    PT_ASSERT(contains(crate_draw, "\"depth_order\":0,\"visibility\":\"frustum-only\""),
              "nearer mesh is depth 0 with frustum-only evidence");
    PT_ASSERT(contains(barrel_draw, "\"depth_order\":1,\"visibility\":\"frustum-only\""),
              "farther mesh is depth 1");
    // world は登録した変換。 行ベクトル行優先 = 列ベクトル列優先 (平行移動は 12〜14 番目)。
    PT_ASSERT(contains(crate_draw, "\"world\":[1,0,0,0,0,1,0,0,0,0,1,0,-2,0,0,1]"), "crate world matrix");
    PT_ASSERT(contains(crate_draw, "\"screen_bbox\":[4"), "crate projects into the left half");
    PT_ASSERT(contains(l, "\"view\":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,-10,1]"), "camera view translation at 12..14");
}

void test_renderer_content_hash() {
    RendererConfig cfg;
    cfg.initial_profile  = "Standard";
    cfg.overlay_mode     = OverlayMode::OFF;
    cfg.frame_tap        = "off";
    std::vector<std::string> lines;
    PictorRenderer renderer;
    renderer.initialize(cfg);

    const float    vertices[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const uint32_t indices[]  = {0, 1, 2};
    MeshDataDescriptor mesh;
    mesh.vertex_data      = vertices;
    mesh.vertex_data_size = sizeof(vertices);
    mesh.vertex_count     = 3;
    mesh.index_data       = indices;
    mesh.index_data_size  = sizeof(indices);
    mesh.index_count      = 3;
    const MeshHandle before = renderer.register_mesh_data(mesh);  // タップ OFF: ハッシュしない
    renderer.frame_tap().set_sink(collecting_sink(lines));
    const MeshHandle after = renderer.register_mesh_data(mesh);   // タップ ON: 内容ハッシュ

    std::string id;
    PT_ASSERT(resolve_mesh_id(renderer.frame_tap_names().mesh_info(before), id) ==
              FrameTapIdentity::COUNT_HASH, "mesh registered while off is count-hash");
    PT_ASSERT(resolve_mesh_id(renderer.frame_tap_names().mesh_info(after), id) ==
              FrameTapIdentity::CONTENT_HASH && starts_with(id, "ch:"),
              "unnamed mesh registered while on is content-hash");
    renderer.shutdown();
}

} // namespace

int main() {
    test_parse_setting();
    test_sink_creation();
    test_projection();
    test_asset_ids();
    test_json_line_encoding();
    test_depth_order_and_lines();
    test_stream_sequence();
    test_observer();
    test_clock();
    test_identity();
    test_generations();
    test_dropped();
    test_ui_semantics();
    test_pass_kinds();
    test_visibility_evidence();
    test_renderer_fixed_scene();
    test_renderer_content_hash();
    return report("unit_frame_tap_test");
}
