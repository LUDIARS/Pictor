#include "raw_frame_writer.h"

namespace pictor_fbx_viewer {

bool RawFrameWriter::open_file(const std::string& path) {
    close();
    file_ = std::fopen(path.c_str(), "wb");
    failed_ = false;
    frames_ = 0;
    return file_ != nullptr;
}

/// @implements SPEC-PC-FBX-TRACK-PLAYBACK
bool RawFrameWriter::write_frame(const uint8_t* pixels, uint32_t width, uint32_t height, bool rgba) {
    if (!file_ || !pixels || failed_) return false;
    const size_t row_bytes = static_cast<size_t>(width) * 4;
    if (!rgba) {
        const size_t bytes = row_bytes * height;
        failed_ = std::fwrite(pixels, 1, bytes, file_) != bytes;
    } else {
        if (row_.size() != row_bytes) row_.resize(row_bytes);
        for (uint32_t y = 0; y < height && !failed_; ++y) {
            const uint8_t* src = pixels + static_cast<size_t>(y) * row_bytes;
            for (size_t x = 0; x < row_bytes; x += 4) {
                row_[x + 0] = src[x + 2];
                row_[x + 1] = src[x + 1];
                row_[x + 2] = src[x + 0];
                row_[x + 3] = src[x + 3];
            }
            failed_ = std::fwrite(row_.data(), 1, row_bytes, file_) != row_bytes;
        }
    }
    if (!failed_) ++frames_;
    return !failed_;
}

bool RawFrameWriter::close() {
    if (!file_) return !failed_;
    const bool flushed = std::fflush(file_) == 0;
    const bool closed  = std::fclose(file_) == 0;
    file_ = nullptr;
    return flushed && closed && !failed_;
}

} // namespace pictor_fbx_viewer
