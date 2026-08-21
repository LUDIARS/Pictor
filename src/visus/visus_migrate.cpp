#include "pictor/visus/visus_migrate.h"

#include "pictor/visus/visus_serializer.h"
#include "visus_json.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace pictor {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kSuffix = ".visus.json";

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

VisusMigrateResult fail(std::string path, std::string msg) {
    VisusMigrateResult r;
    r.path    = std::move(path);
    r.status  = VisusMigrateResult::Status::FAILED;
    r.message = std::move(msg);
    return r;
}

std::string diagnostic_text(std::string_view text) {
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

bool read_bounded(const fs::path& path, std::string& text, std::string& error) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) { error = "open failed"; return false; }
    const std::streamsize size = in.tellg();
    if (size < 0) { error = "size query failed"; return false; }
    if (size > static_cast<std::streamsize>(visus_json::kMaxInputBytes)) {
        error = "visus json is too large";
        return false;
    }
    text.assign(static_cast<size_t>(size), '\0');
    in.seekg(0, std::ios::beg);
    if (size > 0 && !in.read(text.data(), size)) {
        error = "read failed";
        return false;
    }
    return true;
}

bool replace_file(const fs::path& target, std::string_view text, std::string& error) {
    fs::path stage_dir = target;
    stage_dir += ".pictor-migrate.tmp";
    std::error_code ec;
    if (!fs::create_directory(stage_dir, ec) || ec) {
        error = "temporary staging directory already exists or cannot be created";
        return false;
    }
    const fs::path staged = stage_dir / target.filename();
    auto cleanup = [&] {
        std::error_code ignored;
        fs::remove(staged, ignored);
        fs::remove(stage_dir, ignored);
    };

    std::ofstream out(staged, std::ios::binary | std::ios::trunc);
    if (!out) {
        cleanup();
        error = "temporary write failed";
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    out.close();
    if (!out) {
        cleanup();
        error = "temporary write failed";
        return false;
    }

#ifdef _WIN32
    const bool replaced = MoveFileExW(staged.c_str(), target.c_str(),
                                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    fs::rename(staged, target, ec);
    const bool replaced = !ec;
#endif
    if (!replaced) {
        cleanup();
        error = "atomic replace failed";
        return false;
    }
    fs::remove(stage_dir, ec);
    return true;
}

} // namespace

VisusMigrateResult visus_migrate_file(const std::string& path, bool dry_run) {
    // NUL チェックは fs::path を組む前に行う。 embedded NUL を含む文字列から
    // 作った path は NUL 手前で切れた別のファイルを指すため、 先に stat すると
    // 「呼び出し側が渡したのとは違うファイル」 を検査したことになる。
    if (path.empty() || path.find('\0') != std::string::npos)
        return fail(path, "invalid visus path");

    // ディレクトリ走査と同じ symlink 契約を単ファイル経路にも掛ける。 リンクを
    // 辿って読むと置換 (rename) がリンク自身を潰し、 リンク先の内容を無関係な
    // 場所へ書き出したのと同じ結果になる。 symlink_status はリンクを辿らないので
    // リンク自身の種別が取れる。
    const fs::path source(path);
    std::error_code status_ec;
    const fs::file_status status = fs::symlink_status(source, status_ec);
    if (status_ec) return fail(path, "file status failed");
    if (fs::is_symlink(status)) return fail(path, "symbolic link refused");
    if (!fs::is_regular_file(status)) return fail(path, "not a regular file");

    std::string text;
    std::string err;
    if (!read_bounded(source, text, err)) return fail(path, std::move(err));

    VisusValue root;
    if (!visus_json::parse(text, root, &err)) return fail(path, "parse error: " + err);
    if (!root.is_object()) return fail(path, "root is not an object");

    VisusMigrateResult r;
    r.path = path;
    // `version` を落とした v1 文書を v2 と誤認して「移行済み」と報告しないよう、
    // 読込経路と同じ判定を使う (visus_document_version)。
    double version = 2.0;
    if (!visus_document_version(root.as_object(), version, &err))
        return fail(path, std::move(err));
    if (version >= 2.0) {
        r.status = VisusMigrateResult::Status::ALREADY_V2;
        return r;
    }

    VisusDesc desc;
    if (!visus_desc_from_value(root, desc, &err, &r.warnings)) {
        return fail(path, "convert error: " + err);
    }
    // ファイル名 stem を name の既定にする (catalog と同じ規則)。
    if (desc.name.empty()) {
        std::string fn = source.filename().generic_string();
        if (ends_with(fn, kSuffix)) fn.resize(fn.size() - kSuffix.size());
        desc.name = fn;
    }
    r.new_json = to_visus_json(desc);
    r.status   = VisusMigrateResult::Status::MIGRATED;

    if (!dry_run) {
        if (!replace_file(source, r.new_json, err))
            return fail(path, std::move(err));
    }
    return r;
}

std::vector<VisusMigrateResult> visus_migrate_directory(const std::string& dir,
                                                        bool               dry_run,
                                                        bool               recursive) {
    std::vector<VisusMigrateResult> results;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        results.push_back(fail(dir, "not a directory"));
        return results;
    }
    struct Candidate {
        fs::path    path;
        std::string error;
    };
    std::vector<Candidate> candidates;
    auto collect = [&](const fs::directory_entry& ent) {
        if (!ends_with(ent.path().filename().generic_string(), kSuffix)) return;
        std::error_code entry_ec;
        if (ent.is_symlink(entry_ec)) {
            candidates.push_back({ent.path(), "symbolic link refused"});
            return;
        }
        if (entry_ec) {
            candidates.push_back({ent.path(), "file status failed"});
            return;
        }
        if (ent.is_regular_file(entry_ec)) {
            candidates.push_back({ent.path(), {}});
        } else if (entry_ec) {
            candidates.push_back({ent.path(), "file status failed"});
        }
    };
    if (recursive) {
        fs::recursive_directory_iterator it(dir, ec), end;
        while (it != end && !ec) {
            collect(*it);
            it.increment(ec);
        }
    } else {
        fs::directory_iterator it(dir, ec), end;
        while (it != end && !ec) {
            collect(*it);
            it.increment(ec);
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.path < b.path;
    });
    results.reserve(candidates.size() + (ec ? 1u : 0u));
    for (const Candidate& candidate : candidates) {
        results.push_back(candidate.error.empty()
            ? visus_migrate_file(candidate.path.generic_string(), dry_run)
            : fail(candidate.path.generic_string(), candidate.error));
    }
    if (ec) results.push_back(fail(dir, "directory enumeration failed"));
    return results;
}

std::string visus_migrate_summary_line(const VisusMigrateResult& r) {
    std::string line;
    switch (r.status) {
        case VisusMigrateResult::Status::MIGRATED:   line = "migrated   "; break;
        case VisusMigrateResult::Status::ALREADY_V2: line = "already-v2 "; break;
        case VisusMigrateResult::Status::FAILED:     line = "FAILED     "; break;
    }
    line += diagnostic_text(r.path);
    if (!r.message.empty()) line += " (" + r.message + ")";
    if (!r.warnings.empty()) {
        line += " [" + std::to_string(r.warnings.size()) + " warning" +
                (r.warnings.size() == 1 ? "" : "s") + "]";
    }
    return line;
}

} // namespace pictor
