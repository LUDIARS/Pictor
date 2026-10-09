// Frame tap (spec/feature/frame-tap.md) — 設定解析 / 配送先 / AABB 投影 / 安定 ID /
// 1 行 JSON / 深度順 / OFF 時の無出力 / PictorRenderer の固定シーン (メッシュ 2 + UI 矩形 1)。
// 契約 id は spec/tasks/2026-10-09-frame-tap.md の C-1〜C-8。

#include "pictor/core/pictor_renderer.h"
#include "pictor/core/transform_math.h"
#include "pictor/tap/frame_tap.h"
#include "pictor/tap/frame_tap_asset_ids.h"
#include "pictor/tap/frame_tap_config.h"
#include "pictor/tap/frame_tap_json_line.h"
#include "pictor/tap/frame_tap_name_table.h"
#include "pictor/tap/frame_tap_projection.h"
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

    // 視点面をまたぐ箱: 前側の隅だけなら中央の小さな矩形になるが、 視点の後ろへ
    // 広がる辺を w = ε で切ると画面全体へ広がる。
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

// ---- C-4 -----------------------------------------------------------------

void test_asset_ids() {
    std::string id;
    FrameTapMeshInfo named;
    named.name = "crate";
    PT_ASSERT(resolve_mesh_id(named, id), "named mesh resolves by name");
    PT_ASSERT(id == "crate", "mesh id is the asset name");

    FrameTapMeshInfo unnamed;
    unnamed.vertex_count = 3;
    unnamed.index_count  = 6;
    PT_ASSERT(!resolve_mesh_id(unnamed, id), "unnamed mesh reports missing name");
    PT_ASSERT(id == "fp:07e2b7092dc71880", "fingerprint is FNV-1a 64 of vertex+index counts");
    PT_ASSERT(frame_tap_mesh_fingerprint(3, 6) != frame_tap_mesh_fingerprint(6, 3),
              "fingerprint depends on the order of counts");
    PT_ASSERT(std::string(frame_tap_ui_mesh_id(FrameTapUiKind::NINE_SLICE)) == "ui:nine_slice",
              "ui kind id");
}

// ---- C-5 -----------------------------------------------------------------

void test_json_line_encoding() {
    FrameTapFrame frame;
    frame.frame = 7;
    frame.t     = 0.5;
    frame.camera.viewport_width  = 4;
    frame.camera.viewport_height = 2;
    FrameTapDraw draw;
    draw.pass        = FrameTapPass::SCENE;
    draw.mesh        = "a\"b";
    draw.materials   = {"m"};
    draw.instance    = 3;
    draw.screen_bbox = {1.0f, 2.0f, 3.0f, 4.0f};
    draw.tags        = {"unnamed"};
    frame.draws.push_back(draw);
    frame.draw_count = 1;

    std::string line;
    encode_frame_tap_line(frame, line);
    const std::string I = "[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]";
    const std::string expected =
        "{\"frame\":7,\"t\":0.5,\"camera\":{\"view\":" + I + ",\"projection\":" + I +
        ",\"viewport\":[4,2]},\"passes\":[{\"pass\":\"scene\",\"draws\":[{\"mesh\":\"a\\\"b\","
        "\"material\":[\"m\"],\"instance\":3,\"world\":" + I +
        ",\"screen_bbox\":[1,2,3,4],\"depth_order\":0,\"tags\":[\"unnamed\"]}]},"
        "{\"pass\":\"ui\",\"draws\":[]}]}";
    PT_ASSERT(line == expected, "frame encodes to the contract line");
    if (line != expected) std::fprintf(stderr, "       got: %s\n", line.c_str());
    PT_ASSERT(line.find('\n') == std::string::npos, "line has no newline");

    // 有限でない値は偽の 0 ではなく null。 draw_count 外の slot は出さない。
    frame.draws[0].screen_bbox.x = std::numeric_limits<float>::quiet_NaN();
    frame.draws.push_back(draw);
    encode_frame_tap_line(frame, line);
    PT_ASSERT(contains(line, "\"screen_bbox\":[null,2,3,4]"), "non-finite number is null");
    PT_ASSERT(count_of(line, "\"mesh\"") == 1, "only draw_count draws are emitted");
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

    FrameTapCamera camera;
    camera.viewport_width  = 100;
    camera.viewport_height = 100;
    tap.begin_frame(1, 0.0, camera);

    FrameTapSceneObject far_obj;
    far_obj.mesh = 0;
    far_obj.object = 10;
    far_obj.world_bounds = box(-0.1f, -0.1f, -20.0f, 0.1f, 0.1f, -20.0f);
    FrameTapSceneObject near_obj = far_obj;
    near_obj.mesh = 1;
    near_obj.object = 11;
    near_obj.world_bounds = box(-0.1f, -0.1f, -2.0f, 0.1f, 0.1f, -2.0f);
    tap.add_scene_draw(far_obj);
    tap.add_scene_draw(near_obj);

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
        const size_t far_at  = l.find("{\"mesh\":\"far\",\"material\":[],\"instance\":10,");
        const size_t near_at = l.find("{\"mesh\":\"near\",\"material\":[],\"instance\":11,");
        PT_ASSERT(far_at != std::string::npos && near_at != std::string::npos, "scene draws present");
        if (far_at != std::string::npos && near_at != std::string::npos) {
            PT_ASSERT(contains(l.substr(far_at, l.find('}', far_at) - far_at), "\"depth_order\":1"),
                      "far scene draw is behind");
            PT_ASSERT(contains(l.substr(near_at, l.find('}', near_at) - near_at), "\"depth_order\":0"),
                      "near scene draw is front");
        }
        const size_t ui_back = l.find("{\"mesh\":\"ui:rect\"");
        PT_ASSERT(ui_back != std::string::npos &&
                  contains(l.substr(ui_back, l.find('}', ui_back) - ui_back), "\"depth_order\":1"),
                  "earlier ui draw is behind the later one");
        PT_ASSERT(l.find("\"mesh\":\"near\"") < l.find("\"pass\":\"ui\""), "scene pass precedes ui");
        PT_ASSERT(contains(l, "\"mesh\":\"ui:image\",\"material\":[\"hp_bar\"],\"instance\":1,"),
                  "ui texture name is the material id");
    }
    PT_ASSERT_OP(tap.frames_written(), ==, uint64_t{1}, "frames_written counts lines");

    // 名前の無いメッシュ / マテリアルは指紋 + unnamed (ハンドル番号を出さない)。
    lines.clear();
    tap.begin_frame(2, 0.1, camera);
    FrameTapSceneObject anon = near_obj;
    anon.mesh     = 99;
    anon.material = 4;
    tap.add_scene_draw(anon);
    tap.end_frame();
    PT_ASSERT(lines.size() == 1 && contains(lines[0], "\"mesh\":\"fp:") &&
              contains(lines[0], "\"material\":[]") && contains(lines[0], "\"unnamed\""),
              "unnamed draw uses fingerprint and unnamed tag");

    // フレーム外の draw は捨てて数える。
    tap.add_ui_draw(back);
    PT_ASSERT_OP(tap.dropped_draws(), ==, uint64_t{1}, "draw outside a frame is dropped");

    // C-7: 配送先が無ければ何も出さない。
    lines.clear();
    tap.disable();
    PT_ASSERT(!tap.is_enabled(), "disabled tap reports off");
    tap.begin_frame(3, 0.2, camera);
    tap.add_scene_draw(near_obj);
    tap.end_frame();
    PT_ASSERT(lines.empty(), "disabled tap writes nothing");
    PT_ASSERT_OP(tap.dropped_draws(), ==, uint64_t{1}, "disabled tap does not count drops");
}

// ---- C-8: 固定シーン (メッシュ 2 + UI 矩形 1) ---------------------------------

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
    if (tap_on) renderer.frame_tap().set_sink(collecting_sink(run.lines));

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

    renderer.begin_frame(1.0f / 60.0f);
    renderer.render(camera);
    // UIRenderer::record() がタップへ流すのと同じ入口 (headless なので直接呼ぶ)。
    FrameTapUiDraw hud;
    hud.kind = FrameTapUiKind::RECT;
    hud.x = 16.0f; hud.y = 16.0f; hud.w = 200.0f; hud.h = 24.0f;
    renderer.frame_tap().add_ui_draw(hud);
    renderer.end_frame();

    run.visible = renderer.get_frame_stats().visible_objects;
    run.batches = renderer.get_frame_stats().batch_count;
    PT_ASSERT(id_a != id_b, "two distinct objects");
    renderer.shutdown();
    return run;
}

void test_renderer_fixed_scene() {
    const SceneRun off = run_fixed_scene(false);
    const SceneRun on  = run_fixed_scene(true);

    PT_ASSERT(off.lines.empty(), "tap off emits nothing");
    PT_ASSERT_OP(on.visible, ==, off.visible, "tap does not change visibility");
    PT_ASSERT_OP(on.batches, ==, off.batches, "tap does not change batching");
    PT_ASSERT_OP(on.lines.size(), ==, size_t{1}, "one line for one frame");
    if (on.lines.size() != 1) return;

    const std::string& l = on.lines[0];
    PT_ASSERT(l.rfind("{\"frame\":1,\"t\":0.016", 0) == 0, "frame number and elapsed t lead the line");
    PT_ASSERT(contains(l, "\"viewport\":[1280,720]"), "camera viewport");
    PT_ASSERT(contains(l, "\"passes\":[{\"pass\":\"scene\",\"draws\":[") &&
              contains(l, "{\"pass\":\"ui\",\"draws\":[{\"mesh\":\"ui:rect\",\"material\":[],"
                          "\"instance\":0,"), "scene then ui pass");
    PT_ASSERT_OP(count_of(l, "\"mesh\":"), ==, size_t{3}, "2 scene draws + 1 ui draw");
    PT_ASSERT(contains(l, "\"mesh\":\"crate\",\"material\":[\"wood\"]"), "crate named ids");
    PT_ASSERT(contains(l, "\"mesh\":\"barrel\",\"material\":[\"iron\"]"), "barrel named ids");
    PT_ASSERT(!contains(l, "unnamed") && !contains(l, "offscreen"), "no fallback tags");
    PT_ASSERT(contains(l, "\"screen_bbox\":[16,16,200,24]"), "ui bbox is its rect");

    // crate (深度 10) が barrel (深度 15) より前。
    const size_t crate_at  = l.find("\"mesh\":\"crate\"");
    const size_t barrel_at = l.find("\"mesh\":\"barrel\"");
    PT_ASSERT(crate_at != std::string::npos && barrel_at != std::string::npos, "both meshes");
    if (crate_at != std::string::npos && barrel_at != std::string::npos) {
        const std::string crate_draw  = l.substr(crate_at, l.find('}', crate_at) - crate_at);
        const std::string barrel_draw = l.substr(barrel_at, l.find('}', barrel_at) - barrel_at);
        PT_ASSERT(contains(crate_draw, "\"depth_order\":0"), "nearer mesh is depth 0");
        PT_ASSERT(contains(barrel_draw, "\"depth_order\":1"), "farther mesh is depth 1");
        // world は登録した変換 (平行移動は 12〜14 番目)。
        PT_ASSERT(contains(crate_draw, "\"world\":[1,0,0,0,0,1,0,0,0,0,1,0,-2,0,0,1]"),
                  "crate world matrix");
        PT_ASSERT(contains(crate_draw, "\"screen_bbox\":[4"), "crate projects into the left half");
    }
}

} // namespace

int main() {
    test_parse_setting();
    test_sink_creation();
    test_projection();
    test_asset_ids();
    test_json_line_encoding();
    test_depth_order_and_lines();
    test_renderer_fixed_scene();
    return report("unit_frame_tap_test");
}
