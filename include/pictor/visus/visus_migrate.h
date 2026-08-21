#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pictor {

// ============================================================
// visus_migrate — v1 `*.visus.json` を v2 へ書き戻す
// ============================================================
// `spec/feature/visus-v2-design.md` §3.3。 読込は serializer の v1 変換経路
// そのまま、 書き出しは canonical v2。 `dry_run` なら書かずに結果だけ返す。
// CLI (`tools/visus_migrate`) はこの関数の薄いラッパ。

struct VisusMigrateResult {
    enum class Status : uint8_t {
        MIGRATED   = 0,   // v1 → v2 に変換 (dry_run なら「変換できる」)
        ALREADY_V2 = 1,   // version >= 2、 何もしない
        FAILED     = 2    // 読めない / 書けない
    };

    std::string              path;
    Status                   status = Status::FAILED;
    std::string              message;    // FAILED の理由
    std::vector<std::string> warnings;   // 変換時の注意 (handle / remote_url 捨てなど)
    std::string              new_json;   // MIGRATED のとき v2 本文 (dry_run の確認用)
};

/// 1 ファイルを移行。
VisusMigrateResult visus_migrate_file(const std::string& path, bool dry_run);

/// `dir` 直下 (recursive なら配下全部) の `*.visus.json` を移行。 決定的順序。
std::vector<VisusMigrateResult> visus_migrate_directory(const std::string& dir,
                                                        bool               dry_run,
                                                        bool               recursive = false);

/// 結果 1 行の人間可読サマリ ("migrated  path (2 warnings)" 等)。
std::string visus_migrate_summary_line(const VisusMigrateResult& r);

} // namespace pictor
