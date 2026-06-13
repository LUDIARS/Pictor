#include "pictor/profiler/gpu_timer.h"

#include <algorithm>
#include <cstdio>

#ifdef PICTOR_HAS_VULKAN
#include "pictor/surface/vulkan_context.h"
#endif

namespace pictor {

GpuTimerManager::GpuTimerManager() : GpuTimerManager(Config{}) {}

GpuTimerManager::GpuTimerManager(const Config& config)
    : config_(config)
{
    timestamps_.reserve(config.max_queries);
    regions_.reserve(config.max_queries / 2);
}

GpuTimerManager::~GpuTimerManager() {
#ifdef PICTOR_HAS_VULKAN
    destroy_pools_();
#endif
}

#ifdef PICTOR_HAS_VULKAN

bool GpuTimerManager::initialize_vulkan(VulkanContext& vk) {
    if (vulkan_ready_) return true;

    VkPhysicalDevice phys = vk.physical_device();
    VkDevice         dev  = vk.device();
    if (phys == VK_NULL_HANDLE || dev == VK_NULL_HANDLE) {
        std::fprintf(stderr, "[gpu_timer] initialize_vulkan: device 未初期化\n");
        return false;
    }

    // timestampPeriod (nanoseconds per tick) を取得する。 0 なら GPU が
    // graphics/compute キューで timestamp をサポートしていない — シミュレーション
    // 経路のまま継続させる (ブート継続を保証)。
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    if (props.limits.timestampPeriod <= 0.0f) {
        std::fprintf(stderr,
                     "[gpu_timer] timestamp 非対応 GPU (timestampPeriod=0) — "
                     "シミュレーション経路で継続\n");
        return false;
    }
    config_.timestamp_period = props.limits.timestampPeriod;

    // flight_count 枚の query pool を生成する。 各 pool は max_queries 個の
    // TIMESTAMP クエリを持つ。 begin_frame が flight をローテーションし、
    // collect_results が「今書いている flight 以外」の pool から結果を回収する。
    const uint32_t flights = std::max<uint32_t>(1, config_.flight_count);
    query_pools_.assign(flights, VK_NULL_HANDLE);
    flight_frames_.assign(flights, FlightFrame{});

    for (uint32_t i = 0; i < flights; ++i) {
        VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        ci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        ci.queryCount = config_.max_queries;
        if (vkCreateQueryPool(dev, &ci, nullptr, &query_pools_[i]) != VK_SUCCESS) {
            std::fprintf(stderr,
                         "[gpu_timer] vkCreateQueryPool 失敗 (flight %u)\n", i);
            destroy_pools_();
            return false;
        }
    }

    device_       = dev;
    vulkan_ready_ = true;
    std::printf("[gpu_timer] Vulkan timestamp 有効 (flights=%u queries=%u "
                "period=%.3fns)\n",
                flights, config_.max_queries, config_.timestamp_period);
    return true;
}

void GpuTimerManager::shutdown() {
    // device 破棄前に owner が明示的に呼ぶ解放経路 (デストラクタと同処理、冪等)。
    destroy_pools_();
}

void GpuTimerManager::destroy_pools_() {
    if (device_ != VK_NULL_HANDLE) {
        for (VkQueryPool p : query_pools_) {
            if (p != VK_NULL_HANDLE) vkDestroyQueryPool(device_, p, nullptr);
        }
    }
    query_pools_.clear();
    flight_frames_.clear();
    device_       = VK_NULL_HANDLE;
    vulkan_ready_ = false;
}

#endif // PICTOR_HAS_VULKAN

void GpuTimerManager::begin_frame(uint32_t frame_index) {
    current_flight_ = frame_index % std::max<uint32_t>(1, config_.flight_count);
    current_query_ = 0;
    timestamps_.clear();
    regions_.clear();
#ifdef PICTOR_HAS_VULKAN
    reset_done_ = false;
#endif
}

uint32_t GpuTimerManager::write_timestamp(const std::string& label) {
    if (current_query_ >= config_.max_queries) {
        return UINT32_MAX;
    }
    uint32_t query_index = current_query_++;
    timestamps_.push_back({label, query_index, 0, false});
    return query_index;
}

void GpuTimerManager::begin_region(const std::string& name) {
    uint32_t begin_query = write_timestamp(name + "_begin");
    regions_.push_back({name, begin_query, UINT32_MAX, 0.0});
}

void GpuTimerManager::end_region(const std::string& name) {
    uint32_t end_query = write_timestamp(name + "_end");
    for (auto& region : regions_) {
        if (region.name == name && region.end_query == UINT32_MAX) {
            region.end_query = end_query;
            break;
        }
    }
}

#ifdef PICTOR_HAS_VULKAN

void GpuTimerManager::reset_pool(VkCommandBuffer cmd) {
    if (!vulkan_ready_ || reset_done_) return;
    // timestamp クエリは使用前に reset 必須。 今フレームが書き込む pool
    // (current_flight_) を 0..max_queries で reset する。
    vkCmdResetQueryPool(cmd, query_pools_[current_flight_], 0,
                        config_.max_queries);
    reset_done_ = true;
}

uint32_t GpuTimerManager::write_timestamp(const std::string& label,
                                          VkCommandBuffer cmd) {
    uint32_t query_index = write_timestamp(label);
    if (!vulkan_ready_ || query_index == UINT32_MAX) return query_index;
    // 念のため: 最初の timestamp 前に reset を済ませる (呼び側が
    // reset_pool を忘れても安全側に倒す)。
    reset_pool(cmd);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        query_pools_[current_flight_], query_index);
    return query_index;
}

void GpuTimerManager::begin_region(const std::string& name,
                                   VkCommandBuffer cmd) {
    uint32_t begin_query = write_timestamp(name + "_begin");
    regions_.push_back({name, begin_query, UINT32_MAX, 0.0});
    if (!vulkan_ready_ || begin_query == UINT32_MAX) return;
    reset_pool(cmd);
    // 領域開始は TOP_OF_PIPE — 後続コマンドの開始時刻を測る。
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        query_pools_[current_flight_], begin_query);
}

void GpuTimerManager::end_region(const std::string& name,
                                 VkCommandBuffer cmd) {
    uint32_t end_query = write_timestamp(name + "_end");
    for (auto& region : regions_) {
        if (region.name == name && region.end_query == UINT32_MAX) {
            region.end_query = end_query;
            break;
        }
    }
    if (!vulkan_ready_ || end_query == UINT32_MAX) return;
    // 領域終了は BOTTOM_OF_PIPE — それまでの全コマンドの完了時刻を測る。
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        query_pools_[current_flight_], end_query);
}

#endif // PICTOR_HAS_VULKAN

void GpuTimerManager::collect_results() {
#ifdef PICTOR_HAS_VULKAN
    if (vulkan_ready_) {
        // まず今フレームの計測記録を flight_frames_ に確定させる
        // (begin_frame → write*() の後、 次の begin_frame の前に呼ばれる)。
        if (!regions_.empty() || current_query_ > 0) {
            FlightFrame& slot = flight_frames_[current_flight_];
            slot.regions     = regions_;
            slot.query_count = current_query_;
            slot.pending     = true;
        }

        // 結果回収は flight_count フレーム遅延でローテーションされた
        // 過去フレームの pool から行う。 collect_results は次フレームの
        // begin_frame の直前に呼ばれるので、 current_flight_ は「直近に
        // 書いた flight」。 1 周前 (flights 分前) の pool は GPU 完了済みの
        // 可能性が高く、 非ブロッキング読み出しが成立する。
        const uint32_t flights = std::max<uint32_t>(1, config_.flight_count);
        const uint32_t read_flight = (current_flight_ + 1u) % flights;
        FlightFrame& rf = flight_frames_[read_flight];
        if (rf.pending && rf.query_count > 0) {
            std::vector<uint64_t> values(rf.query_count, 0);
            // WAIT_BIT は付けない — まだ完了していなければ NOT_READY が返り、
            // 古い (= 0) 値のまま据え置く。 レンダースレッドをブロックしない。
            VkResult vr = vkGetQueryPoolResults(
                device_, query_pools_[read_flight],
                0, rf.query_count,
                values.size() * sizeof(uint64_t), values.data(),
                sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT);
            if (vr == VK_SUCCESS) {
                // tick 差 → ミリ秒。 region ごとに begin/end query の値差を取る。
                for (auto& region : rf.regions) {
                    if (region.begin_query < rf.query_count &&
                        region.end_query   < rf.query_count) {
                        uint64_t b = values[region.begin_query];
                        uint64_t e = values[region.end_query];
                        double ticks = (e >= b) ? static_cast<double>(e - b) : 0.0;
                        region.elapsed_ms =
                            ticks * static_cast<double>(config_.timestamp_period)
                            / 1e6;
                    }
                }
                // 回収できた結果を resolved_regions_ に確定する。 begin_frame は
                // regions_ をクリアするが resolved_regions_ は据え置くので、
                // get_region_ms / get_all_timings はこれを読む。
                resolved_regions_ = rf.regions;
                rf.pending        = false;
            }
            // NOT_READY のときは何もしない (次の collect で再試行)。
        }
        return;
    }
#endif
    // ── シミュレーション経路 (Vulkan 無効 / 非対応 GPU) ──────────────────
    for (auto& ts : timestamps_) {
        ts.has_result = true;
    }
    for (auto& region : regions_) {
        if (region.begin_query < timestamps_.size() &&
            region.end_query < timestamps_.size() &&
            timestamps_[region.begin_query].has_result &&
            timestamps_[region.end_query].has_result) {
            uint64_t begin_val = timestamps_[region.begin_query].value;
            uint64_t end_val = timestamps_[region.end_query].value;
            region.elapsed_ms = static_cast<double>(end_val - begin_val) *
                               config_.timestamp_period / 1e6;
        }
    }
    // 公開用スナップショットを更新する (begin_frame の clear に耐える)。
    resolved_regions_ = regions_;
}

double GpuTimerManager::get_region_ms(const std::string& name) const {
    for (const auto& region : resolved_regions_) {
        if (region.name == name) return region.elapsed_ms;
    }
    return 0.0;
}

std::vector<GpuTimerManager::RegionTiming> GpuTimerManager::get_all_timings() const {
    std::vector<RegionTiming> result;
    result.reserve(resolved_regions_.size());
    for (const auto& region : resolved_regions_) {
        result.push_back({region.name, region.elapsed_ms});
    }
    return result;
}

} // namespace pictor
