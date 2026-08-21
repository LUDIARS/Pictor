#pragma once

#include "pictor/core/file_watch.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace pictor {

// ============================================================
// PipelineHotReload — パイプライン設定 / シェーダのホットリロード管制
// ============================================================
// `spec/feature/pipeline-hot-reload.md`。
//
// 対象は 「ファイルの変更で構造が変わるもの」 だけ:
//   - pipeline profile JSON (`*.profile.json`) → PictorRenderer::
//     reload_profile_from_source() (系統A→B の既存 seam を再実行)
//   - compiled SPIR-V (`*.spv`)               → ShaderRegistry::
//     rebuild_pipelines() / PostProcessPipeline::rebuild_chain() 等
//
// 値の変更 (uniform / effect パラメータ / profile 内の数値) は
// **ホットリロードしない**。 値はプログラムから既存 API で流し込む
// (register_custom_profile + reload_active_profile /
//  PostProcessPipeline::set_config / push constant)。 これは方針
// (neco 2026-08-20): 「値の変更はホットリロードせずプログラムで対応」。
//
// 仕組みは group 名付きの mtime ポーリング:
//   1. `watch(path, group)` / `watch_directory(dir, ext, group)` で登録
//   2. `on_group(group, cb)` で group ごとの反応を配線
//   3. ホストがフレームループで `poll()` を呼ぶ (interval で間引く)
//
// Pictor は最下層なのでスレッドも OS 監視 API も持たない。 コールバックは
// `poll()` を呼んだスレッド (= ホストのフレームループ) 上で同期実行される。
// GPU リソースを作り直すコールバック側で vkDeviceWaitIdle 等の同期を行う
// こと (ShaderRegistry::rebuild_pipelines は内部で wait idle する)。

class PipelineHotReload {
public:
    /// group ごとの反応。 `paths` はこの poll で変更が確定した該当 group の
    /// ファイル (1 回の poll で複数まとまることがある)。
    using GroupCallback = std::function<void(const std::vector<std::string>& paths)>;

    /// よく使う group 名 (規約であり強制ではない)。
    static constexpr const char* kGroupProfile = "profile";
    static constexpr const char* kGroupShader  = "shader";

    PipelineHotReload() = default;

    /// `path` を `group` で監視する。 登録時点の mtime が基準。
    void watch(const std::string& path, const std::string& group);

    /// `dir` 直下 (recursive なら配下全部) の拡張子 `ext` (例 ".spv") の
    /// 通常ファイルをまとめて `group` で監視する。 symlink は拒否する。
    /// 戻り値は追加した件数。
    /// 監視開始後に増えたファイルは拾わない (再度呼べば追加される)。
    size_t watch_directory(const std::string& dir,
                           const std::string& ext,
                           const std::string& group,
                           bool               recursive = false);

    /// group の反応を設定する (group ごとに 1 つ、 再設定は上書き)。
    void on_group(const std::string& group, GroupCallback cb);

    /// ポーリング間引き間隔 (既定 500ms)。 0 = 毎回。
    void set_poll_interval_ms(uint64_t ms) { interval_ms_ = ms; }

    /// 書き込み settle 待ち (FileWatchSet::set_settle_ms への転送、 既定 200ms)。
    void set_settle_ms(uint64_t ms);

    struct PollResult {
        /// 発火した group 名 (コールバック実行済み)。
        std::vector<std::string> fired_groups;
        /// 変更が確定した全ファイル。
        std::vector<std::string> changed_paths;
        bool any() const { return !changed_paths.empty(); }
    };

    /// 変更を検出し、 該当 group のコールバックを同期実行する。
    /// `now_ms` は単調増加時刻。 省略 (0) なら steady_clock から取る。
    PollResult poll(uint64_t now_ms = 0);

    size_t watch_count() const;

private:
    struct Group {
        FileWatchSet  files;
        GroupCallback callback;
    };

    static uint64_t steady_now_ms_();

    std::map<std::string, Group> groups_;
    uint64_t                     interval_ms_  = 500;
    uint64_t                     settle_ms_    = 200;
    uint64_t                     last_poll_ms_ = 0;
    bool                         has_polled_   = false;
};

} // namespace pictor
