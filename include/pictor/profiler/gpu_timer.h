#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace pictor {

/// GPU timestamp query manager (§13.3).
/// Manages VkQueryPool for GPU timing. Uses flight buffering for async result readback.
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

    /// Begin a new frame's timing
    void begin_frame(uint32_t frame_index);

    /// Record a timestamp (returns query index)
    uint32_t write_timestamp(const std::string& label);

    /// Begin/end a named timer region
    void begin_region(const std::string& name);
    void end_region(const std::string& name);

    /// Collect results from previous frame (non-blocking)
    void collect_results();

    /// Get elapsed time for a named region (ms)
    double get_region_ms(const std::string& name) const;

    /// Get all region timings
    struct RegionTiming {
        std::string name;
        double      gpu_ms;
    };

    std::vector<RegionTiming> get_all_timings() const;

    uint32_t query_count() const { return static_cast<uint32_t>(timestamps_.size()); }

private:
    struct TimestampEntry {
        std::string label;
        uint32_t    query_index;
        uint64_t    value = 0;
        bool        has_result = false;
    };

    struct Region {
        std::string name;
        uint32_t    begin_query;
        uint32_t    end_query;
        double      elapsed_ms = 0.0;
    };

    Config config_;
    std::vector<TimestampEntry> timestamps_;
    std::vector<Region> regions_;
    uint32_t current_query_ = 0;
    uint32_t current_flight_ = 0;
};

} // namespace pictor
