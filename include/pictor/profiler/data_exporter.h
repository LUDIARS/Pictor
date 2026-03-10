#pragma once

#include "pictor/profiler/profiler.h"
#include <string>
#include <vector>
#include <fstream>

namespace pictor {

/// Profiler data export (§13.7).
/// Supports JSON, Chrome Tracing, and CSV formats.
class DataExporter {
public:
    DataExporter() = default;
    ~DataExporter();

    /// Begin recording frames for export
    void begin_recording(const std::string& base_path);

    /// Record a frame's stats
    void record_frame(const FrameStats& stats, uint64_t frame_number);

    /// Stop recording and flush
    void end_recording();

    /// Export as JSON (§13.7)
    bool export_json(const std::string& path) const;

    /// Export as Chrome Tracing format (§13.7)
    bool export_chrome_tracing(const std::string& path) const;

    /// Export as CSV (§13.7)
    bool export_csv(const std::string& path) const;

    bool is_recording() const { return recording_; }

private:
    struct RecordedFrame {
        uint64_t   frame_number;
        FrameStats stats;
    };

    std::vector<RecordedFrame> recorded_frames_;
    std::string base_path_;
    bool recording_ = false;
};

} // namespace pictor
