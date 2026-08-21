#include "pictor/pipeline/pipeline_hot_reload.h"

#include <chrono>
#include <filesystem>
#include <system_error>
#include <utility>

namespace pictor {

namespace fs = std::filesystem;

uint64_t PipelineHotReload::steady_now_ms_() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void PipelineHotReload::watch(const std::string& path, const std::string& group) {
    Group& g = groups_[group];
    g.files.set_settle_ms(settle_ms_);
    g.files.add(path);
}

size_t PipelineHotReload::watch_directory(const std::string& dir,
                                          const std::string& ext,
                                          const std::string& group,
                                          bool               recursive) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;

    Group& watched_group = groups_[group];
    watched_group.files.set_settle_ms(settle_ms_);

    size_t added = 0;
    auto consider = [&](const fs::directory_entry& ent) {
        std::error_code entry_ec;
        if (ent.is_symlink(entry_ec) || entry_ec) return;
        if (!ent.is_regular_file(entry_ec) || entry_ec) return;
        if (ent.path().extension().generic_string() != ext) return;
        const std::string p = ent.path().generic_string();
        if (!watched_group.files.contains(p)) {
            watched_group.files.add(p);
            ++added;
        }
    };
    if (recursive) {
        fs::recursive_directory_iterator it(dir, ec), end;
        while (it != end && !ec) {
            consider(*it);
            it.increment(ec);
        }
    } else {
        fs::directory_iterator it(dir, ec), end;
        while (it != end && !ec) {
            consider(*it);
            it.increment(ec);
        }
    }
    return added;
}

void PipelineHotReload::on_group(const std::string& group, GroupCallback cb) {
    groups_[group].callback = std::move(cb);
}

void PipelineHotReload::set_settle_ms(uint64_t ms) {
    settle_ms_ = ms;
    for (auto& [_, g] : groups_) g.files.set_settle_ms(ms);
}

size_t PipelineHotReload::watch_count() const {
    size_t n = 0;
    for (const auto& [_, g] : groups_) n += g.files.size();
    return n;
}

PipelineHotReload::PollResult PipelineHotReload::poll(uint64_t now_ms) {
    if (now_ms == 0) now_ms = steady_now_ms_();

    PollResult result;
    if (has_polled_ && interval_ms_ > 0 && now_ms - last_poll_ms_ < interval_ms_) {
        return result;   // 間引き
    }
    has_polled_   = true;
    last_poll_ms_ = now_ms;

    for (auto& [name, g] : groups_) {
        std::vector<std::string> changed = g.files.poll_changed(now_ms);
        if (changed.empty()) continue;
        result.changed_paths.insert(result.changed_paths.end(),
                                    changed.begin(), changed.end());
        result.fired_groups.push_back(name);
        // A callback may replace its own registration. Invoke a stable copy so
        // that mutation cannot destroy the std::function currently executing.
        GroupCallback callback = g.callback;
        if (callback) callback(changed);
    }
    return result;
}

} // namespace pictor
