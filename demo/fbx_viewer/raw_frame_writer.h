// `--raw-out <path|->` sink (SPEC-PC-FBX-TRACK-PLAYBACK): every rendered
// frame as raw BGRA8, width * height * 4 bytes, top row first, no header.
// RGBA swapchains are converted through one reusable row buffer, so
// steady-state writes do not allocate.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pictor_fbx_viewer {

class RawFrameWriter {
public:
    RawFrameWriter() = default;
    RawFrameWriter(const RawFrameWriter&) = delete;
    RawFrameWriter& operator=(const RawFrameWriter&) = delete;
    ~RawFrameWriter() { close(); }

    /// Open a regular file for writing (truncates).
    bool open_file(const std::string& path);
    /// Take ownership of an already opened binary stream (diverted stdout).
    void attach(std::FILE* stream) { close(); file_ = stream; }
    bool is_open() const { return file_ != nullptr; }

    /// Write one frame of tightly packed pixels. `rgba` swaps R and B.
    bool write_frame(const uint8_t* pixels, uint32_t width, uint32_t height, bool rgba);
    uint64_t frames_written() const { return frames_; }

    /// Flush and close; returns false when any write or the close failed.
    bool close();

private:
    std::FILE*           file_ = nullptr;
    std::vector<uint8_t> row_;
    uint64_t             frames_ = 0;
    bool                 failed_ = false;
};

} // namespace pictor_fbx_viewer
