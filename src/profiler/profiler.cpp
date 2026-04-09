#include "pictor/profiler/profiler.h"
#include <numeric>
#include <algorithm>

namespace pictor {

Profiler::Profiler() {
    fps_samples_.fill(0.0);
    frame_time_history_.fill(0.0);
    gpu_time_history_.fill(0.0);
}

Profiler::~Profiler() = default;

void Profiler::begin_frame() {
    if (!enabled_) return;

    frame_timer_.start();
    current_stats_ = {}; // reset for new frame
    cpu_sections_.clear();

    // Collect GPU results from previous frame (async readback, §13.3)
    gpu_timer_.collect_results();
    gpu_timer_.begin_frame(static_cast<uint32_t>(frame_count_));
}

void Profiler::end_frame() {
    if (!enabled_) return;

    frame_timer_.stop();
    double frame_ms = frame_timer_.elapsed_ms();

    // Update current stats
    current_stats_.frame_time_ms = frame_ms;

    // Collect CPU section timings
    for (const auto& section : cpu_sections_) {
        double ms = section.timer.elapsed_ms();
        if (section.name == "DataUpdate")     current_stats_.data_update_ms = ms;
        else if (section.name == "Culling")   current_stats_.culling_ms = ms;
        else if (section.name == "Sort")      current_stats_.sort_ms = ms;
        else if (section.name == "BatchBuild") current_stats_.batch_build_ms = ms;
        else if (section.name == "CommandEncode") current_stats_.command_encode_ms = ms;
    }

    // Collect GPU timings
    current_stats_.shadow_gpu_ms        = gpu_timer_.get_region_ms("ShadowPass");
    current_stats_.depth_prepass_gpu_ms  = gpu_timer_.get_region_ms("DepthPrePass");
    current_stats_.opaque_gpu_ms        = gpu_timer_.get_region_ms("OpaquePass");
    current_stats_.transparent_gpu_ms   = gpu_timer_.get_region_ms("TransparentPass");
    current_stats_.post_process_gpu_ms  = gpu_timer_.get_region_ms("PostProcess");
    current_stats_.compute_update_gpu_ms = gpu_timer_.get_region_ms("ComputeUpdate");
    current_stats_.gpu_cull_ms          = gpu_timer_.get_region_ms("GPUCullPass");

    current_stats_.gpu_frame_time_ms = current_stats_.shadow_gpu_ms +
                                       current_stats_.depth_prepass_gpu_ms +
                                       current_stats_.opaque_gpu_ms +
                                       current_stats_.transparent_gpu_ms +
                                       current_stats_.post_process_gpu_ms +
                                       current_stats_.compute_update_gpu_ms +
                                       current_stats_.gpu_cull_ms;

    // FPS calculation — moving average over N frames (§13.2)
    fps_samples_[fps_sample_index_] = frame_ms;
    fps_sample_index_ = (fps_sample_index_ + 1) % FPS_SAMPLE_COUNT;

    double avg_frame_time = 0.0;
    for (size_t i = 0; i < FPS_SAMPLE_COUNT; ++i) {
        avg_frame_time += fps_samples_[i];
    }
    avg_frame_time /= FPS_SAMPLE_COUNT;
    avg_fps_ = (avg_frame_time > 0.0) ? (1000.0 / avg_frame_time) : 0.0;
    current_stats_.fps = avg_fps_;

    // Update history for graphs (§13.2)
    frame_time_history_[history_index_] = frame_ms;
    gpu_time_history_[history_index_] = current_stats_.gpu_frame_time_ms;
    history_index_ = (history_index_ + 1) % HISTORY_SIZE;

    // Store as last stats
    last_stats_ = current_stats_;
    ++frame_count_;
}

void Profiler::begin_cpu_section(const std::string& name) {
    if (!enabled_) return;
    CpuSection section;
    section.name = name;
    section.timer.start();
    cpu_sections_.push_back(section);
}

void Profiler::end_cpu_section(const std::string& name) {
    if (!enabled_) return;
    for (auto& section : cpu_sections_) {
        if (section.name == name) {
            section.timer.stop();
            break;
        }
    }
}

void Profiler::begin_gpu_section(const std::string& name) {
    if (!enabled_) return;
    gpu_timer_.begin_region(name);
}

void Profiler::end_gpu_section(const std::string& name) {
    if (!enabled_) return;
    gpu_timer_.end_region(name);
}

void Profiler::record_memory_stats(size_t frame_used, size_t frame_cap,
                                    size_t pool_used,
                                    size_t gpu_ssbo, size_t gpu_mesh,
                                    size_t gpu_total, size_t gpu_cap) {
    current_stats_.frame_alloc_used     = frame_used;
    current_stats_.frame_alloc_capacity = frame_cap;
    current_stats_.pool_used            = pool_used;
    current_stats_.gpu_ssbo_used        = gpu_ssbo;
    current_stats_.gpu_mesh_pool_used   = gpu_mesh;
    current_stats_.gpu_total_used       = gpu_total;
    current_stats_.gpu_total_capacity   = gpu_cap;
}

} // namespace pictor
