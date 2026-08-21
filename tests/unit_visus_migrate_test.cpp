/// visus_migrate — v1 *.visus.json を v2 へ書き戻す (dry-run / 実書き / 冪等)。

#include "pictor/visus/visus_migrate.h"
#include "pictor/visus/visus_serializer.h"
#include "test_common.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace pictor;
using namespace pictor_test;

namespace fs = std::filesystem;

namespace {

const char* kV1 = R"({
  "version": 1,
  "name": "rabbit",
  "geometry": {
    "kind": "model",
    "asset": { "local_path": "../../KzSUnity/Assets/3D/Characters/ch_KnifeRabbit/KnifeUsagi.fbx",
               "remote_url": "", "sha256": "", "size_bytes": 0, "fetch_policy": "cache_first", "headers": [] },
    "rive_artboard": "", "text_default": "",
    "mesh": "none", "model": "handle:3", "shader": "none", "generic_handle": 0
  },
  "materials": [],
  "textures": [
    { "slot": "diffuse", "texture": "none",
      "resource": { "local_path": "../../KzSUnity/Assets/3D/Characters/ch_KnifeRabbit/T_Knifeusagi_Albedo.png" } }
  ],
  "flags": { "default_flags": 2, "layer": 0, "pool_hint": "dynamic", "initial_lod": 0 },
  "animation_default": { "kind": "clip", "name": "Idle", "loop": true, "speed": 1 },
  "shader_key_override": 0
})";

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_all(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary);
    out << s;
}

void test_migrate_directory() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "pictor_unit_visus_migrate";
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "sub", ec);

    write_all(dir / "rabbit.visus.json", kV1);
    write_all(dir / "noname.visus.json", "{\"version\":1,\"geometry\":{\"kind\":\"rive\",\"asset\":{\"local_path\":\"x.riv\"}}}");
    write_all(dir / "already.visus.json", "{\"version\":2,\"name\":\"already\",\"kind\":\"group\"}");
    write_all(dir / "broken.visus.json", "{ \"version\": 1, ");
    write_all(dir / "sub" / "deep.visus.json", kV1);
    write_all(dir / "notes.json", "{}");

    // dry-run: 何も書かない
    auto dry = visus_migrate_directory(dir.generic_string(), /*dry_run=*/true);
    PT_ASSERT_OP(dry.size(), ==, size_t{4}, "4 *.visus.json in dir (non-recursive)");
    int migrated = 0, already = 0, failed = 0;
    for (const auto& r : dry) {
        if (r.status == VisusMigrateResult::Status::MIGRATED)   ++migrated;
        if (r.status == VisusMigrateResult::Status::ALREADY_V2) ++already;
        if (r.status == VisusMigrateResult::Status::FAILED)     ++failed;
    }
    PT_ASSERT(migrated == 2 && already == 1 && failed == 1, "dry-run classifies 2 migrated / 1 already / 1 failed");
    PT_ASSERT(read_all(dir / "rabbit.visus.json") == kV1, "dry-run leaves file untouched");
    for (const auto& r : dry) {
        if (r.path.find("rabbit") != std::string::npos) {
            PT_ASSERT(r.new_json.find("\"version\": 2") != std::string::npos, "dry-run exposes v2 body");
            PT_ASSERT(!r.warnings.empty(), "dry-run reports conversion warnings");
            PT_ASSERT(visus_migrate_summary_line(r).rfind("migrated", 0) == 0, "summary line prefix");
        }
        if (r.path.find("noname") != std::string::npos)
            PT_ASSERT(r.new_json.find("\"name\": \"noname\"") != std::string::npos, "empty name takes file stem");
    }

    // 実書き (recursive)
    auto real = visus_migrate_directory(dir.generic_string(), /*dry_run=*/false, /*recursive=*/true);
    PT_ASSERT_OP(real.size(), ==, size_t{5}, "recursive includes sub/");
    const std::string after = read_all(dir / "rabbit.visus.json");
    PT_ASSERT(after.find("\"version\": 2") != std::string::npos, "file rewritten as v2");
    PT_ASSERT(after.find("handle:") == std::string::npos, "handles dropped on disk");
    VisusDesc d;
    PT_ASSERT(from_visus_json(after, d), "rewritten file parses");
    PT_ASSERT(d.name == "rabbit" && d.kind == VisusKind::MODEL &&
              d.metadata.get_string("texture.diffuse").has_value() &&
              d.metadata.get_string(visus_keys::kAnimationDefault).value_or("") == "Idle",
              "content preserved through migration");
    PT_ASSERT(read_all(dir / "sub" / "deep.visus.json").find("\"version\": 2") != std::string::npos, "recursive file rewritten");

    // 冪等: 2 回目は全部 already-v2 (broken は失敗のまま)
    auto again = visus_migrate_directory(dir.generic_string(), false, true);
    int a2 = 0, f2 = 0;
    for (const auto& r : again) {
        if (r.status == VisusMigrateResult::Status::ALREADY_V2) ++a2;
        if (r.status == VisusMigrateResult::Status::FAILED)     ++f2;
    }
    PT_ASSERT(a2 == 4 && f2 == 1, "second run is idempotent");

    // 置換前の staging が作れない場合も、元 v1 ファイルは一切変更しない。
    const fs::path guarded = dir / "guarded.visus.json";
    fs::path stage_dir = guarded;
    stage_dir += ".pictor-migrate.tmp";
    write_all(guarded, kV1);
    fs::create_directory(stage_dir, ec);
    const VisusMigrateResult guarded_result = visus_migrate_file(guarded.generic_string(), false);
    PT_ASSERT(guarded_result.status == VisusMigrateResult::Status::FAILED,
              "staging collision fails before replacement");
    PT_ASSERT(read_all(guarded) == kV1, "failed replacement preserves original source exactly");
    fs::remove(stage_dir, ec);
    fs::remove(guarded, ec);

    VisusMigrateResult unsafe_summary;
    unsafe_summary.path = "bad\nname.visus.json";
    unsafe_summary.status = VisusMigrateResult::Status::FAILED;
    PT_ASSERT(visus_migrate_summary_line(unsafe_summary).find("\\x0a") != std::string::npos,
              "summary escapes terminal control characters in untrusted paths");

    constexpr char nul_path_data[] = "prefix\0suffix.visus.json";
    const std::string nul_path(nul_path_data, sizeof(nul_path_data) - 1);
    PT_ASSERT(visus_migrate_file(nul_path, true).status == VisusMigrateResult::Status::FAILED,
              "embedded NUL migration path is rejected before filesystem access");

    // 単一ファイル API も directory scan と同じく symlink を追従しない。
    // 作成権限がない Windows 環境では、このケースだけを skip する。
    const fs::path link_target = dir / "link-target.visus.json";
    const fs::path link = dir / "linked.visus.json";
    write_all(link_target, kV1);
    ec.clear();
    fs::create_symlink(link_target, link, ec);
    if (!ec) {
        const VisusMigrateResult linked = visus_migrate_file(link.generic_string(), false);
        PT_ASSERT(linked.status == VisusMigrateResult::Status::FAILED &&
                  linked.message.find("symbolic link") != std::string::npos,
                  "direct migration refuses a symbolic link");
        PT_ASSERT(fs::is_symlink(link, ec), "refused migration preserves the link");
        PT_ASSERT(read_all(link_target) == kV1, "refused migration preserves the target");
        fs::remove(link, ec);
    }
    fs::remove(link_target, ec);

    // 単ファイル / 不在
    PT_ASSERT(visus_migrate_file((dir / "nope.visus.json").generic_string(), true).status == VisusMigrateResult::Status::FAILED,
              "missing file fails");
    PT_ASSERT(visus_migrate_directory((dir / "nodir").generic_string(), true).front().status == VisusMigrateResult::Status::FAILED,
              "missing dir fails");

    fs::remove_all(dir, ec);
}

// `version` を落とした v1 文書は、 読込経路 (from_visus_json) が v1 固有の
// トップレベル key で v1 と判定する。 migrate が同じ判定を共有していないと
// 「already-v2」と報告して黙って移行を飛ばす。
void test_version_less_v1_is_migrated() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "pictor_unit_visus_migrate_versionless";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    const fs::path legacy = dir / "legacy.visus.json";
    write_all(legacy, "{\"name\":\"legacy\",\"geometry\":{\"kind\":\"model\","
                      "\"asset\":{\"local_path\":\"a.fbx\"},\"model\":\"handle:3\"}}");
    // v1 固有 key が無ければ version 欠落は v2 扱い (読込経路と同じ)。
    const fs::path modern = dir / "modern.visus.json";
    write_all(modern, "{\"name\":\"modern\",\"kind\":\"group\"}");

    const VisusMigrateResult legacy_dry = visus_migrate_file(legacy.generic_string(), true);
    PT_ASSERT(legacy_dry.status == VisusMigrateResult::Status::MIGRATED,
              "version-less v1 is detected as v1, not reported already-v2");
    PT_ASSERT(legacy_dry.new_json.find("\"version\": 2") != std::string::npos,
              "version-less v1 dry-run produces a v2 body");
    PT_ASSERT(visus_migrate_file(modern.generic_string(), true).status ==
              VisusMigrateResult::Status::ALREADY_V2,
              "version-less document without v1-only keys stays v2");

    PT_ASSERT(visus_migrate_file(legacy.generic_string(), false).status ==
              VisusMigrateResult::Status::MIGRATED, "version-less v1 is rewritten");
    const std::string after = read_all(legacy);
    PT_ASSERT(after.find("\"version\": 2") != std::string::npos, "version stamped on disk");
    PT_ASSERT(after.find("handle:") == std::string::npos, "resolved handle dropped");
    VisusDesc d;
    PT_ASSERT(from_visus_json(after, d) && d.name == "legacy" && d.kind == VisusKind::MODEL,
              "rewritten version-less v1 round-trips as v2");
    PT_ASSERT(visus_migrate_file(legacy.generic_string(), false).status ==
              VisusMigrateResult::Status::ALREADY_V2, "second pass is a no-op");

    fs::remove_all(dir, ec);
}

} // namespace

int main() {
    test_migrate_directory();
    test_version_less_v1_is_migrated();
    return report("unit_visus_migrate_test");
}
