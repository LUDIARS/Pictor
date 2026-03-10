#pragma once

#include "pictor/core/types.h"
#include "pictor/profiler/cpu_timer.h"
#include "pictor/profiler/gpu_timer.h"
#include <vector>
#include <string>
#include <array>

namespace pictor {

/// Frame statistics (§13.2, §13.4, §13.5)
struct FrameStats {
    // FPS / Frame Time (§13.2)
    double   fps              = 0.0;
    double   frame_time_ms    = 0.0;
    double   gpu_frame_time_ms = 0.0;

    // Per-pass CPU times (§13.3)
    double   data_update_ms   = 0.0;
    double   culling_ms       = 0.0;
    double   sort_ms          = 0.0;
    double   batch_build_ms   = 0.0;
    double   command_encode_ms = 0.0;

    // Per-pass GPU times (§13.3)
    double   shadow_gpu_ms    = 0.0;
    double   depth_prepass_gpu_ms = 0.0;
    double   opaque_gpu_ms    = 0.0;
    double   transparent_gpu_ms = 0.0;
    double   post_process_gpu_ms = 0.0;
    double   compute_update_gpu_ms = 0.0;
    double   gpu_cull_ms      = 0.0;

    // Draw statistics (§13.5)
    uint32_t draw_call_count  = 0;
    uint64_t triangle_count   = 0;
    uint32_t batch_count      = 0;
    uint32_t visible_objects  = 0;
    uint32_t culled_objects   = 0;
    uint32_t gpu_driven_objects = 0;
    uint32_t compute_update_objects = 0;

    // Memory statistics (§13.4)
    size_t   frame_alloc_used     = 0;
    size_t   frame_alloc_capacity = 0;
    size_t   pool_used            = 0;
    size_t   gpu_ssbo_used        = 0;
    size_t   gpu_mesh_pool_used   = 0;
    size_t   gpu_total_used       = 0;
    size_t   gpu_total_capacity   = 0;
};

/// Built-in profiler (§13).
/// Low-overhead design suitable for release builds.
class Profiler {
public:
    Profiler();
    ~Profiler();

    /// Enable/disable profiler (§12)
    void set_enabled(bool enabled) { enabled_ = enabled; }
    bool is_enabled() const { return enabled_; }

    /// Set overlay mode (§13.6)
    void set_overlay_mode(OverlayMode mode) { overlay_mode_ = mode; }
    OverlayMode overlay_mode() const { return overlay_mode_; }

    /// Frame lifecycle
    void begin_frame();
    void end_frame();

    /// Begin/end a named CPU timing section
    void begin_cpu_section(const std::string& name);
    void end_cpu_section(const std::string& name);

    /// Record GPU timestamps
    void begin_gpu_section(const std::string& name);
    void end_gpu_section(const std::string& name);

    /// Update draw stats
    void record_draw_calls(uint32_t count) { current_stats_.draw_call_count += count; }
    void record_triangles(uint64_t count) { current_stats_.triangle_count += count; }
    void record_batches(uint32_t count) { current_stats_.batch_count = count; }
    void record_visible(uint32_t visible, uint32_t culled) {
        current_stats_.visible_objects = visible;
        current_stats_.culled_objects = culled;
    }
    void record_gpu_driven_objects(uint32_t count) { current_stats_.gpu_driven_objects = count; }
    void record_compute_update_objects(uint32_t count) { current_stats_.compute_update_objects = count; }

    /// Update memory stats
    void record_memory_stats(size_t frame_used, size_t frame_cap,
                             size_t pool_used,
                             size_t gpu_ssbo, size_t gpu_mesh,
                             size_t gpu_total, size_t gpu_cap);

    /// Get current/previous frame stats (§12)
    const FrameStats& get_frame_stats() const { return last_stats_; }

    /// Get frame time history for graph display (§13.2)
    static constexpr size_t HISTORY_SIZE = 300;
    const std::array<double, HISTORY_SIZE>& frame_time_history() const { return frame_time_history_; }
    const std::array<double, HISTORY_SIZE>& gpu_time_history() const { return gpu_time_history_; }

    /// FPS calculation (§13.2)
    static constexpr size_t FPS_SAMPLE_COUNT = 60;
    double average_fps() const { return avg_fps_; }

    /// GPU timer manager access
    GpuTimerManager& gpu_timer() { return gpu_timer_; }

private:
    bool         enabled_ = true;
    OverlayMode  overlay_mode_ = OverlayMode::STANDARD;

    // Frame stats
    FrameStats   current_stats_;
    FrameStats   last_stats_;

    // CPU timing
    struct CpuSection {
        std::string name;
        CpuTimer    timer;
    };
    std::vector<CpuSection> cpu_sections_;

    // GPU timing
    GpuTimerManager gpu_timer_;

    // FPS calculation
    std::array<double, FPS_SAMPLE_COUNT> fps_samples_;
    size_t fps_sample_index_ = 0;
    double avg_fps_ = 0.0;

    // History for graphs
    std::array<double, HISTORY_SIZE> frame_time_history_{};
    std::array<double, HISTORY_SIZE> gpu_time_history_{};
    size_t history_index_ = 0;

    // Frame timer
    CpuTimer frame_timer_;
    uint64_t frame_count_ = 0;
};

} // namespace pictor
