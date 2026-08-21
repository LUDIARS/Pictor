// visus_migrate — v1 *.visus.json を v2 へ書き戻す小 CLI。
//
//   visus_migrate [--dry-run] [--recursive] [--verbose] <dir-or-file>...
//
// 本体は pictor ライブラリの visus_migrate_directory / visus_migrate_file。
// 終了コード: 0 = 失敗なし、 1 = 1 件以上 FAILED、 2 = 引数不正。

#include "pictor/visus/visus_migrate.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string terminal_safe(std::string_view text) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(text.size());
    for (const unsigned char c : text) {
        if (c < 0x20 || c == 0x7f) {
            out += "\\x";
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0f]);
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    bool dry_run = false, recursive = false, verbose = false;
    std::vector<std::string> targets;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--dry-run") == 0)        dry_run = true;
        else if (std::strcmp(a, "--recursive") == 0) recursive = true;
        else if (std::strcmp(a, "--verbose") == 0)   verbose = true;
        else if (a[0] == '-' && a[1] == '-') {
            const std::string safe = terminal_safe(a);
            std::fprintf(stderr, "unknown option: %s\n", safe.c_str());
            return 2;
        } else targets.emplace_back(a);
    }
    if (targets.empty()) {
        std::fprintf(stderr, "usage: visus_migrate [--dry-run] [--recursive] [--verbose] <dir-or-file>...\n");
        return 2;
    }

    int failed = 0, migrated = 0, already = 0;
    for (const std::string& t : targets) {
        std::vector<pictor::VisusMigrateResult> results;
        std::error_code ec;
        if (std::filesystem::is_directory(t, ec)) {
            results = pictor::visus_migrate_directory(t, dry_run, recursive);
        } else {
            results.push_back(pictor::visus_migrate_file(t, dry_run));
        }
        for (const auto& r : results) {
            std::printf("%s\n", pictor::visus_migrate_summary_line(r).c_str());
            if (verbose) {
                for (const auto& w : r.warnings)
                    std::printf("    - %s\n", terminal_safe(w).c_str());
                if (dry_run && r.status == pictor::VisusMigrateResult::Status::MIGRATED)
                    std::printf("%s", r.new_json.c_str());
            }
            switch (r.status) {
                case pictor::VisusMigrateResult::Status::MIGRATED:   ++migrated; break;
                case pictor::VisusMigrateResult::Status::ALREADY_V2: ++already;  break;
                case pictor::VisusMigrateResult::Status::FAILED:     ++failed;   break;
            }
        }
    }
    std::printf("%s: %d migrated, %d already v2, %d failed\n",
                dry_run ? "dry-run" : "done", migrated, already, failed);
    return failed ? 1 : 0;
}
