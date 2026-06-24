#pragma once

#include "pictor/core/types.h"
#include "pictor/profiler/perf_introspection.h"
#include <vector>
#include <string>

#ifdef PICTOR_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace pictor {

class GpuTimerManager;

/// per-batch GPU 時間の host-bracket ヘルパ。
///
/// Pictor の managed path は大半が host-driven (per-batch の `vkCmdDraw*` は
/// host が発行する) なので、per-batch GPU 時間は **host がコマンドバッファを
/// 握る箇所でしか計測できない**。本ヘルパは `GpuTimerManager` を per-batch の
/// 安定した region 名でラップし、解決済み GPU 時間をバッチメタデータと突き合わせる。
///
/// Pictor が draw を出さないバッチの時間をでっち上げることはしない —
/// 未計測バッチは `gpu_ms = 0` / `measured = false` のまま返す
/// (spec §1 の正直さ原則 / [[feedback_no_silent_fallback]])。
///
/// 使い方 (host = Ergo render layer 等):
///   BatchGpuTimer bt(renderer.profiler().gpu_timer());
///   毎フレーム:
///     bt.begin_frame(batches);            // 今フレームのバッチ列を渡す
///     for (i, batch) :
///       bt.begin_batch(i, cmd);
///       ... そのバッチの draw cmd ...
///       bt.end_batch(i, cmd);
///     ... submit ...
///     auto timings = bt.timings();        // 1〜数フレーム前の解決済み GPU 時間
class BatchGpuTimer {
public:
    explicit BatchGpuTimer(GpuTimerManager& gpu);

    /// フレーム頭で今フレームに描くバッチ列を登録する。region 名のための
    /// 安定領域を必要数まで伸長し、各バッチのメタ (pool/mesh/count) を控える。
    void begin_frame(const std::vector<RenderBatch>& batches);

#ifdef PICTOR_HAS_VULKAN
    /// `cmd` へ実 timestamp を打ってバッチ region を開始/終了する。
    void begin_batch(uint32_t batch_index, VkCommandBuffer cmd);
    void end_batch(uint32_t batch_index, VkCommandBuffer cmd);
#endif
    /// Vulkan 無効ビルド / ヘッドレス用 (timestamp は書かれない)。
    void begin_batch(uint32_t batch_index);
    void end_batch(uint32_t batch_index);

    /// 直近に解決できたフレームの per-batch GPU 時間。
    /// 各 batch_index につき GpuTimerManager から region 時間を引く
    /// (`measured` は region 名が結果に存在したかで決まる)。
    std::vector<BatchTimingInfo> timings() const;

    /// このフレームで bracket 対象として登録したバッチ数。
    uint32_t batch_count() const { return static_cast<uint32_t>(meta_.size()); }

private:
    /// batch_index 用の安定した region 名 ("batch_<index>") を返す。
    const char* region_name_(uint32_t index);

    GpuTimerManager&         gpu_;
    std::vector<std::string> names_;  // region 名の安定ストレージ (const char* の寿命保証)
    std::vector<BatchTimingInfo> meta_; // batch_index / pool / mesh / object_count
};

} // namespace pictor
