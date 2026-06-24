#pragma once

#include "pictor/profiler/perf_introspection.h"
#include "pictor/profiler/hardware_counters.h"
#include <string>
#include <vector>

namespace pictor {

class SceneRegistry;
class MemorySubsystem;
class BatchBuilder;
class Profiler;

/// 性能 & DoD キャッシュ可視化のための read-only facade
/// (`DataQueryAPI` と同じパターン — 非破壊・内部 layout 非公開)。
///
/// レンダーループには一切触れず、既存サブシステムの状態を**読むだけ**で
/// A (構造的レイアウト) / B (パス毎キャッシュトラフィック) / バッチ適格性 /
/// per-batch 時間 / C (HW カウンタ) を集約する。
///
/// 構築 (Ergo 等の host 側):
///   PerfQueryAPI perf = renderer.create_perf_query_api();
///   std::string json = perf.export_json();
///
/// HW カウンタ (C 層) は `set_hardware_counter_provider()` で注入する
/// (未注入なら hw_counters() は available=false を返す)。
/// per-batch GPU 時間は `BatchGpuTimer` 側で host が計測した結果を
/// `set_batch_timings()` で渡す (Pictor が draw を出さないため)。
class PerfQueryAPI {
public:
    PerfQueryAPI(const SceneRegistry& scene,
                 const MemorySubsystem& memory,
                 const BatchBuilder& batches,
                 const Profiler& profiler);
    ~PerfQueryAPI();

    // ---- A. メモリレイアウト / キャッシュ効率 (構造的) ----

    MemoryLayoutReport memory_layout_report() const;

    // ---- B. パス毎キャッシュトラフィック (モデル推定) ----

    std::vector<CacheTrafficInfo> cache_traffic() const;

    // ---- バッチ適格性 (per-object → per-batch) ----

    BatchEligibilityReport batch_eligibility_report() const;

    // ---- per-batch GPU 時間 ----

    /// host (BatchGpuTimer) が計測した per-batch 時間を取り込む。
    void set_batch_timings(std::vector<BatchTimingInfo> timings);
    const std::vector<BatchTimingInfo>& batch_timings() const { return batch_timings_; }

    // ---- C. HW カウンタ ----

    /// HW カウンタ provider を注入 (所有しない)。
    void set_hardware_counter_provider(IHardwareCounterProvider* provider) {
        hw_provider_ = provider;
    }
    /// 直近の HW カウンタサンプルを取得 (provider 未注入/不可なら理由付き unavailable)。
    HwCounterSample hw_counters() const;

    // ---- JSON export (Ergo plugin 契約 — spec §5) ----

    std::string export_json() const;
    std::string export_memory_json() const;
    std::string export_batches_json() const;

private:
    const SceneRegistry&  scene_;
    const MemorySubsystem& memory_;
    const BatchBuilder&   batches_;
    const Profiler&       profiler_;

    IHardwareCounterProvider*  hw_provider_ = nullptr;
    std::vector<BatchTimingInfo> batch_timings_;
};

} // namespace pictor
