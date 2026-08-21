#include "pictor/core/file_watch.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <utility>

namespace pictor {

namespace fs = std::filesystem;

int64_t FileWatchSet::stat_mtime_ns_(const std::string& path) {
    std::error_code ec;
    const fs::file_time_type t = fs::last_write_time(path, ec);
    if (ec) return -1;
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch())
            .count());
}

void FileWatchSet::add(const std::string& path) {
    if (path.empty() || path.find('\0') != std::string::npos) return;
    if (contains(path)) return;
    Entry e;
    e.path     = path;
    e.mtime_ns = stat_mtime_ns_(path);
    entries_.push_back(std::move(e));
}

bool FileWatchSet::contains(const std::string& path) const {
    return std::any_of(entries_.begin(), entries_.end(),
                       [&](const Entry& e) { return e.path == path; });
}

std::vector<std::string> FileWatchSet::poll_changed(uint64_t now_ms) {
    std::vector<std::string> changed;
    for (Entry& e : entries_) {
        const int64_t cur = stat_mtime_ns_(e.path);
        if (cur == e.mtime_ns) {
            // 基準に一致。 pending があっても「元に戻った」 とみなし破棄。
            e.pending_ns = -2;
            continue;
        }
        if (cur != e.pending_ns) {
            // 新しい変化 (最初の検出、 または pending 中にさらに書き換わった)。
            // settle タイマーを仕切り直す。
            e.pending_ns       = cur;
            e.pending_since_ms = now_ms;
            continue;
        }
        // pending と同じ値のまま — settle 待ち。
        if (now_ms - e.pending_since_ms >= settle_ms_) {
            e.mtime_ns   = cur;
            e.pending_ns = -2;
            changed.push_back(e.path);
        }
    }
    return changed;
}

} // namespace pictor
