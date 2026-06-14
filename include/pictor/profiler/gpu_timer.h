#pragma once

#include <cstdint>
#include <vector>
#include <string>

#ifdef PICTOR_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace pictor {

class VulkanContext;

/// 1 region につき 2 本打つ timestamp の begin/end 識別子。
/// 以前は `name + "_begin"` / `name + "_end"` の **per-frame 文字列連結**
/// (長い region 名で SSO 超過 → ヒープ確保) で区別していたが、 enum 化して
/// per-frame アロケーションを撤廃した (review/2026-06-11 D-3)。
enum class TimestampKind : uint8_t {
    Begin = 0,
    End,
};

/// GPU timestamp query manager (§13.3).
///
/// 実 Vulkan の `VkQueryPool` で per-pass の GPU 実行時間を計測する。
/// `flight_count` 枚の query pool をローテーションし、 結果読み出し
/// (`vkGetQueryPoolResults`) は常に 1〜数フレーム前の pool に対して
/// 行うので レンダースレッドをブロックしない。
///
/// 使い方 (ホスト / ゲーム側、 Vulkan 経路):
///   GpuTimerManager gt;
///   gt.initialize_vulkan(vk);                     // 起動時に 1 度
///   ...
///   毎フレーム:
///     gt.collect_results();                       // 前フレームの結果を回収
///     gt.begin_frame(frame_index);
///     gt.begin_region("scene_hdr", cmd);          // cmd へ vkCmdWriteTimestamp
///     ... 描画コマンド ...
///     gt.end_region("scene_hdr", cmd);
///     ... submit / present ...
///
/// `VkCommandBuffer` を取らない `begin_region(name)` / `write_timestamp(name)`
/// は従来互換のシミュレーション経路 (ヘッドレステスト / Vulkan 無効ビルド)。
/// Vulkan 初期化済みでも cmd 無し版はクエリ枠だけ確保し実 timestamp は
/// 書かないので、 計測には cmd 付きオーバーロードを使うこと。
class GpuTimerManager {
public:
    struct Config {
        uint32_t max_queries = 64;    // 32 passes x start/end
        uint32_t flight_count = 3;
        float    timestamp_period = 1.0f; // nanoseconds per tick (device-dependent)
    };

    GpuTimerManager();
    explicit GpuTimerManager(const Config& config);
    ~GpuTimerManager();

    GpuTimerManager(const GpuTimerManager&)            = delete;
    GpuTimerManager& operator=(const GpuTimerManager&) = delete;

#ifdef PICTOR_HAS_VULKAN
    /// 実 Vulkan 計測を有効にする。 `flight_count` 枚の `VkQueryPool`
    /// (各 `max_queries` 個の TIMESTAMP クエリ) を生成し、
    /// `VkPhysicalDeviceLimits::timestampPeriod` を Config に取り込む。
    ///
    /// デバイスが timestamp クエリ非対応 (`timestampComputeAndGraphics`
    /// false 相当 = `timestampPeriod == 0`) なら false を返し、 マネージャは
    /// シミュレーション経路のまま動く (ブート継続を保証)。
    /// 既に初期化済みなら何もせず true。
    bool initialize_vulkan(VulkanContext& vk);

    /// 実 Vulkan 計測が有効か。
    bool is_vulkan_ready() const { return vulkan_ready_; }

    /// 実 Vulkan 計測リソース (query pool) を明示的に解放する。 冪等。
    ///
    /// owner は **VkDevice を破棄する前** に必ず呼ぶこと。 デストラクタ任せに
    /// すると、 owner の破棄順次第で device 破棄後に destroy_pools_ が走り、
    /// `vkDestroyQueryPool: Invalid device` で異常終了する。
    void shutdown();
#endif

    /// Begin a new frame's timing
    void begin_frame(uint32_t frame_index);

    /// Record a timestamp (returns query index)。
    /// `region` / `kind` は debug 識別用で hot path では文字列を作らない
    /// (`const char*` リテラルをそのまま保持)。
    uint32_t write_timestamp(const char* region, TimestampKind kind);

    /// Begin/end a named timer region。
    /// `name` は呼び側が渡す **文字列リテラル** (静的寿命) を想定し、 内部では
    /// `const char*` のまま保持する (per-frame コピー / alloc なし)。
    void begin_region(const char* name);
    void end_region(const char* name);

#ifdef PICTOR_HAS_VULKAN
    /// `cmd` へ実 `vkCmdWriteTimestamp` を発行する計測経路。
    /// `begin_region` は領域開始時 (TOP_OF_PIPE)、 `end_region` は
    /// 領域終了時 (BOTTOM_OF_PIPE) に timestamp を書く。
    uint32_t write_timestamp(const char* region, TimestampKind kind, VkCommandBuffer cmd);
    void begin_region(const char* name, VkCommandBuffer cmd);
    void end_region(const char* name, VkCommandBuffer cmd);

    /// begin_frame の直後・最初の timestamp を書く前に 1 度呼ぶ。
    /// 今フレームが書き込む query pool を `vkCmdResetQueryPool` する
    /// (timestamp クエリは reset 必須)。 Vulkan 未初期化なら no-op。
    void reset_pool(VkCommandBuffer cmd);
#endif

    /// Collect results from previous frame (non-blocking)
    void collect_results();

    /// Get elapsed time for a named region (ms)
    double get_region_ms(const char* name) const;

    /// Get all region timings。
    /// `name` は region 登録時に渡された `const char*` リテラルを指す
    /// (静的寿命なのでコピー不要)。
    struct RegionTiming {
        const char* name;
        double      gpu_ms;
    };

    std::vector<RegionTiming> get_all_timings() const;

    uint32_t query_count() const { return static_cast<uint32_t>(timestamps_.size()); }

private:
    struct TimestampEntry {
        const char*  region = nullptr;       // 文字列リテラル (debug 識別用)
        TimestampKind kind  = TimestampKind::Begin;
        uint32_t     query_index = 0;
        uint64_t     value = 0;
        bool         has_result = false;
    };

    struct Region {
        const char* name;
        uint32_t    begin_query;
        uint32_t    end_query;
        double      elapsed_ms = 0.0;
    };

    /// 1 フレーム分の計測スナップショット。 結果回収を flight_count
    /// フレーム遅延させるため、 各 flight ごとに「どの region がどの
    /// query index を使ったか」を保持する。
    struct FlightFrame {
        std::vector<Region>      regions;
        uint32_t                 query_count = 0;
        bool                     pending     = false;  // 結果未回収
    };

    Config config_;
    std::vector<TimestampEntry> timestamps_;
    std::vector<Region> regions_;          // 今フレームの記録中 region 群
    std::vector<Region> resolved_regions_; // 直近に結果回収できた region 群
    uint32_t current_query_ = 0;
    uint32_t current_flight_ = 0;

#ifdef PICTOR_HAS_VULKAN
    void destroy_pools_();

    bool                       vulkan_ready_ = false;
    VkDevice                   device_       = VK_NULL_HANDLE;
    std::vector<VkQueryPool>    query_pools_;   // flight_count 枚
    std::vector<FlightFrame>    flight_frames_; // flight_count 枚 (計測記録)
    bool                       reset_done_   = false;   // 今フレーム reset 済みか
#endif
};

} // namespace pictor
