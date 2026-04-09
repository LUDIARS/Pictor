#include "pictor/profiler/gpu_timer.h"
#include <algorithm>

namespace pictor {

GpuTimerManager::GpuTimerManager() : GpuTimerManager(Config{}) {}

GpuTimerManager::GpuTimerManager(const Config& config)
    : config_(config)
{
    timestamps_.reserve(config.max_queries);
    regions_.reserve(config.max_queries / 2);
}

GpuTimerManager::~GpuTimerManager() = default;

void GpuTimerManager::begin_frame(uint32_t frame_index) {
    current_flight_ = frame_index % config_.flight_count;
    current_query_ = 0;
    timestamps_.clear();
    regions_.clear();
}

uint32_t GpuTimerManager::write_timestamp(const std::string& label) {
    if (current_query_ >= config_.max_queries) {
        return UINT32_MAX;
    }

    uint32_t query_index = current_query_++;
    timestamps_.push_back({label, query_index, 0, false});

    // In a real Vulkan implementation:
    // vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queryPool, queryIndex)

    return query_index;
}

void GpuTimerManager::begin_region(const std::string& name) {
    uint32_t begin_query = write_timestamp(name + "_begin");
    regions_.push_back({name, begin_query, UINT32_MAX, 0.0});
}

void GpuTimerManager::end_region(const std::string& name) {
    uint32_t end_query = write_timestamp(name + "_end");

    // Find the matching region
    for (auto& region : regions_) {
        if (region.name == name && region.end_query == UINT32_MAX) {
            region.end_query = end_query;
            break;
        }
    }
}

void GpuTimerManager::collect_results() {
    // In a real Vulkan implementation:
    // vkGetQueryPoolResults(device, queryPool[prevFlight], ...)
    // Then compute elapsed time from timestamp differences

    // Mark all timestamps as having results (simulated)
    for (auto& ts : timestamps_) {
        ts.has_result = true;
    }

    // Compute region elapsed times
    for (auto& region : regions_) {
        if (region.begin_query < timestamps_.size() &&
            region.end_query < timestamps_.size() &&
            timestamps_[region.begin_query].has_result &&
            timestamps_[region.end_query].has_result) {
            uint64_t begin_val = timestamps_[region.begin_query].value;
            uint64_t end_val = timestamps_[region.end_query].value;
            // Convert ticks to milliseconds using timestamp_period
            region.elapsed_ms = static_cast<double>(end_val - begin_val) *
                               config_.timestamp_period / 1e6;
        }
    }
}

double GpuTimerManager::get_region_ms(const std::string& name) const {
    for (const auto& region : regions_) {
        if (region.name == name) return region.elapsed_ms;
    }
    return 0.0;
}

std::vector<GpuTimerManager::RegionTiming> GpuTimerManager::get_all_timings() const {
    std::vector<RegionTiming> result;
    result.reserve(regions_.size());
    for (const auto& region : regions_) {
        result.push_back({region.name, region.elapsed_ms});
    }
    return result;
}

} // namespace pictor
