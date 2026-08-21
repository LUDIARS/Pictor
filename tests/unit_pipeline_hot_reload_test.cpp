/// PipelineHotReload — group 分配 / interval 間引き / watch_directory フィルタ。

#include "pictor/pipeline/pipeline_profile.h"
#include "pictor/core/pictor_renderer.h"
#include "pictor/pipeline/pipeline_hot_reload.h"
#include "test_common.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace pictor;
using namespace pictor_test;

namespace fs = std::filesystem;

namespace {

void write_file(const fs::path& p, const char* text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

void bump_mtime(const fs::path& p, int sec) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    fs::last_write_time(p, t + std::chrono::seconds(sec), ec);
}

// symlink も含めた `.spv` エントリ数 (watch_directory が型で弾いたことを示すため)。
size_t spv_entry_count(const fs::path& dir) {
    std::error_code ec;
    size_t n = 0;
    for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (it->path().extension() == ".spv") ++n;
    }
    return n;
}

void test_groups_and_interval() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "pictor_unit_hot_reload";
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "shaders", ec);

    const fs::path profile = dir / "kuzu.profile.json";
    write_file(profile, "{\"profile_name\":\"Kuzu\"}");
    write_file(dir / "shaders" / "a.vert.spv", "aa");
    write_file(dir / "shaders" / "a.frag.spv", "bb");
    write_file(dir / "shaders" / "notes.txt", "not a shader");

    // symlink 作成には Windows で Developer Mode / 管理者権限が要る。 作れた時だけ
    // 「.spv でも symlink なら拾わない」 を検証する (作れなければ拡張子フィルタ
    // のみの検証に落ちる — 黙って通らないよう has_symlink で明示する)。
    std::error_code link_ec;
    fs::create_symlink(dir / "shaders" / "a.vert.spv",
                       dir / "shaders" / "linked.spv", link_ec);
    const bool has_symlink =
        !link_ec && fs::is_symlink(dir / "shaders" / "linked.spv", ec);

    PipelineHotReload hot;
    hot.set_settle_ms(100);
    hot.set_poll_interval_ms(500);
    hot.watch(profile.generic_string(), PipelineHotReload::kGroupProfile);
    const size_t added = hot.watch_directory((dir / "shaders").generic_string(), ".spv",
                                             PipelineHotReload::kGroupShader);
    PT_ASSERT_OP(added, ==, size_t{2}, "directory scan picks only regular .spv files");
    if (has_symlink) {
        // ディレクトリ上の `.spv` は 3 本 (a.vert / a.frag / linked)。 追加が 2 本
        // ということは symlink だけが弾かれた、 の意。
        PT_ASSERT_OP(spv_entry_count(dir / "shaders"), ==, size_t{3},
                     "symlink is a .spv entry, so the filter had to reject it by type");
    }
    PT_ASSERT_OP(hot.watch_count(), ==, size_t{3}, "3 files watched");
    PT_ASSERT_OP(hot.watch_directory((dir / "shaders").generic_string(), ".spv",
                                     PipelineHotReload::kGroupShader), ==, size_t{0},
                 "re-scan without new files adds none");

    int profile_fired = 0, shader_fired = 0;
    std::vector<std::string> last_shader_paths;
    hot.on_group(PipelineHotReload::kGroupProfile,
                 [&](const std::vector<std::string>&) { ++profile_fired; });
    hot.on_group(PipelineHotReload::kGroupShader,
                 [&](const std::vector<std::string>& p) { ++shader_fired; last_shader_paths = p; });

    // 変更なし
    PT_ASSERT(!hot.poll(1000).any(), "quiet poll");

    // interval 間引き: 1000ms の直後は poll 自体がスキップされる
    bump_mtime(profile, 10);
    PT_ASSERT(hot.poll(1100).changed_paths.empty(), "inside interval -> skipped");

    // interval 経過 → settle arm → さらに interval + settle 経過で発火
    PT_ASSERT(hot.poll(1600).changed_paths.empty(), "arm settle");
    const auto r = hot.poll(2200);
    PT_ASSERT_OP(r.fired_groups.size(), ==, size_t{1}, "one group fired");
    PT_ASSERT(r.fired_groups[0] == PipelineHotReload::kGroupProfile, "profile group");
    PT_ASSERT(profile_fired == 1 && shader_fired == 0, "only profile callback ran");

    // shader 2 本を同時に変更 → 1 回の poll でまとまって 1 コールバック
    bump_mtime(dir / "shaders" / "a.vert.spv", 10);
    bump_mtime(dir / "shaders" / "a.frag.spv", 10);
    hot.poll(2800);                      // arm
    const auto r2 = hot.poll(3400);
    PT_ASSERT(r2.fired_groups.size() == 1 && r2.fired_groups[0] == PipelineHotReload::kGroupShader,
              "shader group fired");
    PT_ASSERT_OP(last_shader_paths.size(), ==, size_t{2}, "both spv reported together");
    PT_ASSERT_OP(shader_fired, ==, 1, "one callback for the batch");
    PT_ASSERT_OP(profile_fired, ==, 1, "profile unaffected");

    // interval 0 = 毎回 poll
    hot.set_poll_interval_ms(0);
    bump_mtime(profile, 10);
    hot.poll(3500);
    hot.poll(3650);
    PT_ASSERT_OP(profile_fired, ==, 2, "no throttling with interval 0");

    fs::remove_all(dir, ec);
}

void test_callback_less_group_still_reports() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "pictor_unit_hot_reload2";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path f = dir / "x.spv";
    write_file(f, "s");

    PipelineHotReload hot;
    hot.set_settle_ms(50);
    hot.set_poll_interval_ms(0);
    hot.watch(f.generic_string(), "loose");
    bump_mtime(f, 10);
    hot.poll(100);
    const auto r = hot.poll(200);
    PT_ASSERT(r.any() && r.fired_groups.size() == 1 && r.fired_groups[0] == "loose",
              "group without callback still reported in PollResult");

    fs::remove_all(dir, ec);
}

void test_file_profile_detaches_when_programmatic_state_supersedes_it() {
    std::error_code ec;
    const fs::path profile_path =
        fs::temp_directory_path(ec) / "pictor_hot_file.profile.json";
    write_file(profile_path, "{\"profile_name\":\"HotFile\"}");

    PictorRenderer renderer;
    renderer.initialize();
    std::string error;
    PT_ASSERT(renderer.load_profile_from_file(profile_path.generic_string(), &error),
              error.c_str());
    PT_ASSERT(renderer.profile_source_path() == profile_path.generic_string(),
              "file-backed profile exposes its source");

    PipelineProfileDef replacement = PipelineProfileManager::create_lite_profile();
    replacement.profile_name = "HotFile";
    renderer.register_custom_profile(replacement);
    PT_ASSERT(renderer.profile_source_path().empty(),
              "programmatic replacement detaches the old file source");
    PT_ASSERT(!renderer.reload_profile_from_source(&error),
              "detached source cannot reactivate a stale file profile");

    PT_ASSERT(renderer.load_profile_from_file(profile_path.generic_string(), &error),
              error.c_str());
    PT_ASSERT(renderer.set_profile("Lite"), "programmatic profile switch succeeds");
    PT_ASSERT(renderer.profile_source_path().empty(),
              "programmatic selection detaches the old file source");

    PT_ASSERT(renderer.load_profile_from_file(profile_path.generic_string(), &error),
              error.c_str());
    renderer.shutdown();
    PT_ASSERT(renderer.profile_source_path().empty(),
              "shutdown detaches the file source before reinitialization");

    fs::remove(profile_path, ec);
}

void test_file_profile_reload_uses_the_original_base() {
    std::error_code ec;
    const fs::path profile_path =
        fs::temp_directory_path(ec) / "pictor_hot_reload_base.profile.json";

    PictorRenderer renderer;
    renderer.initialize();
    const uint8_t base_msaa = renderer.profile_manager().current_profile().msaa_samples;
    const uint8_t override_msaa = base_msaa == 4 ? 2 : 4;

    write_file(profile_path,
               override_msaa == 4
                   ? "{\"profile_name\":\"HotBase\",\"msaa_samples\":4}"
                   : "{\"profile_name\":\"HotBase\",\"msaa_samples\":2}");
    std::string error;
    PT_ASSERT(renderer.load_profile_from_file(profile_path.generic_string(), &error),
              error.c_str());
    PT_ASSERT_OP(renderer.profile_manager().current_profile().msaa_samples,
                 ==, override_msaa, "file override is active");

    // Removing the key must restore the value from the base captured by the
    // first load, not inherit the previous file-derived override.
    write_file(profile_path, "{\"profile_name\":\"HotBase\"}");
    PT_ASSERT(renderer.reload_profile_from_source(&error), error.c_str());
    PT_ASSERT_OP(renderer.profile_manager().current_profile().msaa_samples,
                 ==, base_msaa, "deleted override returns to the original base");

    renderer.shutdown();
    fs::remove(profile_path, ec);
}

} // namespace

int main() {
    test_groups_and_interval();
    test_callback_less_group_still_reports();
    test_file_profile_detaches_when_programmatic_state_supersedes_it();
    test_file_profile_reload_uses_the_original_base();
    return report("unit_pipeline_hot_reload_test");
}
