#include "pictor/profiler/data_exporter.h"
#include <sstream>
#include <iomanip>

namespace pictor {

DataExporter::~DataExporter() {
    if (recording_) {
        end_recording();
    }
}

void DataExporter::begin_recording(const std::string& base_path) {
    base_path_ = base_path;
    recorded_frames_.clear();
    recording_ = true;
}

void DataExporter::record_frame(const FrameStats& stats, uint64_t frame_number) {
    if (!recording_) return;
    recorded_frames_.push_back({frame_number, stats});
}

void DataExporter::end_recording() {
    recording_ = false;
}

bool DataExporter::export_json(const std::string& path) const {
    // §13.7: JSON Export
    std::ofstream file(path);
    if (!file.is_open()) return false;

    file << "{\n  \"frames\": [\n";

    for (size_t i = 0; i < recorded_frames_.size(); ++i) {
        const auto& frame = recorded_frames_[i];
        const auto& s = frame.stats;

        file << "    {\n";
        file << "      \"frame\": " << frame.frame_number << ",\n";
        file << "      \"fps\": " << std::fixed << std::setprecision(1) << s.fps << ",\n";
        file << "      \"frame_time_ms\": " << s.frame_time_ms << ",\n";
        file << "      \"gpu_frame_time_ms\": " << s.gpu_frame_time_ms << ",\n";
        file << "      \"data_update_ms\": " << s.data_update_ms << ",\n";
        file << "      \"culling_ms\": " << s.culling_ms << ",\n";
        file << "      \"sort_ms\": " << s.sort_ms << ",\n";
        file << "      \"batch_build_ms\": " << s.batch_build_ms << ",\n";
        file << "      \"command_encode_ms\": " << s.command_encode_ms << ",\n";
        file << "      \"shadow_gpu_ms\": " << s.shadow_gpu_ms << ",\n";
        file << "      \"opaque_gpu_ms\": " << s.opaque_gpu_ms << ",\n";
        file << "      \"transparent_gpu_ms\": " << s.transparent_gpu_ms << ",\n";
        file << "      \"post_process_gpu_ms\": " << s.post_process_gpu_ms << ",\n";
        file << "      \"compute_update_gpu_ms\": " << s.compute_update_gpu_ms << ",\n";
        file << "      \"draw_calls\": " << s.draw_call_count << ",\n";
        file << "      \"triangles\": " << s.triangle_count << ",\n";
        file << "      \"batches\": " << s.batch_count << ",\n";
        file << "      \"visible_objects\": " << s.visible_objects << ",\n";
        file << "      \"culled_objects\": " << s.culled_objects << ",\n";
        file << "      \"gpu_driven_objects\": " << s.gpu_driven_objects << ",\n";
        file << "      \"frame_alloc_used\": " << s.frame_alloc_used << ",\n";
        file << "      \"gpu_total_used\": " << s.gpu_total_used << "\n";
        file << "    }";
        if (i + 1 < recorded_frames_.size()) file << ",";
        file << "\n";
    }

    file << "  ]\n}\n";
    return true;
}

bool DataExporter::export_chrome_tracing(const std::string& path) const {
    // §13.7: Chrome Tracing (chrome://tracing compatible JSON)
    std::ofstream file(path);
    if (!file.is_open()) return false;

    file << "{\"traceEvents\":[\n";

    bool first = true;
    for (const auto& frame : recorded_frames_) {
        const auto& s = frame.stats;
        double frame_start_us = frame.frame_number * s.frame_time_ms * 1000.0;

        auto write_event = [&](const char* name, double start_us, double dur_us, const char* cat) {
            if (!first) file << ",\n";
            first = false;
            file << "  {\"name\":\"" << name << "\","
                 << "\"cat\":\"" << cat << "\","
                 << "\"ph\":\"X\","
                 << "\"ts\":" << std::fixed << std::setprecision(1) << start_us << ","
                 << "\"dur\":" << dur_us << ","
                 << "\"pid\":1,\"tid\":1}";
        };

        double offset = frame_start_us;
        write_event("Frame", offset, s.frame_time_ms * 1000.0, "frame");
        write_event("DataUpdate", offset, s.data_update_ms * 1000.0, "cpu");
        offset += s.data_update_ms * 1000.0;
        write_event("Culling", offset, s.culling_ms * 1000.0, "cpu");
        offset += s.culling_ms * 1000.0;
        write_event("Sort", offset, s.sort_ms * 1000.0, "cpu");
        offset += s.sort_ms * 1000.0;
        write_event("BatchBuild", offset, s.batch_build_ms * 1000.0, "cpu");

        // GPU events on tid 2
        auto write_gpu = [&](const char* name, double start_us, double dur_us) {
            if (!first) file << ",\n";
            first = false;
            file << "  {\"name\":\"" << name << "\","
                 << "\"cat\":\"gpu\","
                 << "\"ph\":\"X\","
                 << "\"ts\":" << std::fixed << std::setprecision(1) << start_us << ","
                 << "\"dur\":" << dur_us << ","
                 << "\"pid\":1,\"tid\":2}";
        };

        double gpu_offset = frame_start_us;
        write_gpu("ComputeUpdate", gpu_offset, s.compute_update_gpu_ms * 1000.0);
        gpu_offset += s.compute_update_gpu_ms * 1000.0;
        write_gpu("Shadow", gpu_offset, s.shadow_gpu_ms * 1000.0);
        gpu_offset += s.shadow_gpu_ms * 1000.0;
        write_gpu("Opaque", gpu_offset, s.opaque_gpu_ms * 1000.0);
        gpu_offset += s.opaque_gpu_ms * 1000.0;
        write_gpu("Transparent", gpu_offset, s.transparent_gpu_ms * 1000.0);
        gpu_offset += s.transparent_gpu_ms * 1000.0;
        write_gpu("PostProcess", gpu_offset, s.post_process_gpu_ms * 1000.0);
    }

    file << "\n]}\n";
    return true;
}

bool DataExporter::export_csv(const std::string& path) const {
    // §13.7: CSV Export
    std::ofstream file(path);
    if (!file.is_open()) return false;

    file << "frame,fps,frame_time_ms,gpu_frame_time_ms,"
         << "data_update_ms,culling_ms,sort_ms,batch_build_ms,"
         << "shadow_gpu_ms,opaque_gpu_ms,transparent_gpu_ms,post_process_gpu_ms,"
         << "compute_update_gpu_ms,"
         << "draw_calls,triangles,batches,visible_objects,culled_objects,"
         << "frame_alloc_used,gpu_total_used\n";

    for (const auto& frame : recorded_frames_) {
        const auto& s = frame.stats;
        file << frame.frame_number << ","
             << std::fixed << std::setprecision(2)
             << s.fps << "," << s.frame_time_ms << "," << s.gpu_frame_time_ms << ","
             << s.data_update_ms << "," << s.culling_ms << ","
             << s.sort_ms << "," << s.batch_build_ms << ","
             << s.shadow_gpu_ms << "," << s.opaque_gpu_ms << ","
             << s.transparent_gpu_ms << "," << s.post_process_gpu_ms << ","
             << s.compute_update_gpu_ms << ","
             << s.draw_call_count << "," << s.triangle_count << ","
             << s.batch_count << "," << s.visible_objects << "," << s.culled_objects << ","
             << s.frame_alloc_used << "," << s.gpu_total_used << "\n";
    }

    return true;
}

} // namespace pictor
