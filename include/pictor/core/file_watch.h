#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pictor {

// ============================================================
// FileWatchSet — mtime ポーリングのファイル変更検出
// ============================================================
// `spec/feature/pipeline-hot-reload.md`。 OS のファイル監視 API には
// 依存しない (Pictor は最下層 — std::filesystem の mtime だけを使い、
// ホストがフレームループから `poll()` を呼ぶ)。
//
// - 変更 = last_write_time の変化、 または存在 ↔ 不在の遷移。
// - 書き込み途中の半端な読み取りを避けるため、 「mtime が変わった」 だけ
//   では発火せず、 変化後 `settle_ms` (既定 200ms) 変化が止まってから
//   changed として報告する (エディタ / コンパイラの連続書き込み対策)。
// - poll ごとの stat 呼び出しは全登録ファイルに対して行う。 数十〜数百
//   ファイル規模を想定 (毎フレームではなく `PipelineHotReload` の
//   interval で間引かれる前提)。

class FileWatchSet {
public:
    /// `path` を監視対象に追加する。 現時点の mtime を基準にする
    /// (追加時点より前の変更は報告しない)。 重複追加は無視。
    void add(const std::string& path);

    bool   contains(const std::string& path) const;
    size_t size() const { return entries_.size(); }
    void   clear() { entries_.clear(); }

    /// 変更が確定した (settle した) ファイルのパスを返し、 基準 mtime を
    /// 更新する。 `now_ms` は単調増加の現在時刻 (ミリ秒)。
    std::vector<std::string> poll_changed(uint64_t now_ms);

    void     set_settle_ms(uint64_t ms) { settle_ms_ = ms; }
    uint64_t settle_ms() const { return settle_ms_; }

private:
    struct Entry {
        std::string path;
        int64_t     mtime_ns     = -1;   // -1 = 不在
        int64_t     pending_ns   = -2;   // -2 = pending 無し
        uint64_t    pending_since_ms = 0;
    };

    static int64_t stat_mtime_ns_(const std::string& path);

    std::vector<Entry> entries_;
    uint64_t           settle_ms_ = 200;
};

} // namespace pictor
