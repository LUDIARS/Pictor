#include "pictor/animation/lottie_animation.h"
#include <fstream>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace pictor {

/// Internal implementation (placeholder for Lottie runtime integration)
struct LottieAnimation::Impl {
    // In a production build, this would wrap rlottie or lottie-cpp
    std::string json_data;
};

LottieAnimation::LottieAnimation() = default;
LottieAnimation::~LottieAnimation() = default;

bool LottieAnimation::load_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        error_ = "Failed to open Lottie file: " + path;
        return false;
    }

    std::string json((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
    return load_json(json);
}

bool LottieAnimation::load_json(const std::string& json_data) {
    if (json_data.empty()) {
        error_ = "Empty Lottie JSON data";
        return false;
    }

    impl_ = std::make_unique<Impl>();
    impl_->json_data = json_data;

    // Parse essential fields from JSON
    // Minimal JSON parsing for: w, h, ip, op, fr, markers
    auto find_number = [&](const std::string& key) -> float {
        std::string search = "\"" + key + "\"";
        auto pos = json_data.find(search);
        if (pos == std::string::npos) return 0.0f;
        pos = json_data.find(':', pos);
        if (pos == std::string::npos) return 0.0f;
        ++pos;
        while (pos < json_data.size() && (json_data[pos] == ' ' || json_data[pos] == '\t')) ++pos;
        try { return std::stof(json_data.substr(pos)); }
        catch (...) { return 0.0f; }
    };

    width_       = find_number("w");
    height_      = find_number("h");
    start_frame_ = find_number("ip");
    end_frame_   = find_number("op");
    frame_rate_  = find_number("fr");

    if (width_ <= 0.0f) width_ = 512.0f;
    if (height_ <= 0.0f) height_ = 512.0f;
    if (frame_rate_ <= 0.0f) frame_rate_ = 30.0f;
    if (end_frame_ <= start_frame_) end_frame_ = start_frame_ + 60.0f;

    current_frame_ = start_frame_;
    segment_start_ = start_frame_;
    segment_end_   = end_frame_;

    // Parse markers (simplified)
    auto marker_pos = json_data.find("\"markers\"");
    if (marker_pos != std::string::npos) {
        // Minimal marker parsing — look for objects with cm, tm, dr fields
        auto bracket = json_data.find('[', marker_pos);
        if (bracket != std::string::npos) {
            auto end_bracket = json_data.find(']', bracket);
            if (end_bracket != std::string::npos) {
                std::string markers_str = json_data.substr(bracket, end_bracket - bracket + 1);
                // Each marker: {"cm":"name","tm":start,"dr":duration}
                size_t search_pos = 0;
                while (true) {
                    auto cm_pos = markers_str.find("\"cm\"", search_pos);
                    if (cm_pos == std::string::npos) break;

                    auto name_start = markers_str.find('"', markers_str.find(':', cm_pos) + 1);
                    auto name_end = markers_str.find('"', name_start + 1);
                    if (name_start == std::string::npos || name_end == std::string::npos) break;

                    LottieMarker marker;
                    marker.name = markers_str.substr(name_start + 1, name_end - name_start - 1);

                    auto tm_pos = markers_str.find("\"tm\"", name_end);
                    if (tm_pos != std::string::npos) {
                        auto colon = markers_str.find(':', tm_pos);
                        try { marker.start_frame = std::stof(markers_str.substr(colon + 1)); }
                        catch (...) {}
                    }

                    auto dr_pos = markers_str.find("\"dr\"", name_end);
                    if (dr_pos != std::string::npos) {
                        auto colon = markers_str.find(':', dr_pos);
                        try {
                            float dur = std::stof(markers_str.substr(colon + 1));
                            marker.end_frame = marker.start_frame + dur;
                        } catch (...) {
                            marker.end_frame = marker.start_frame;
                        }
                    }

                    markers_.push_back(marker);
                    search_pos = name_end + 1;
                }
            }
        }
    }

    loaded_ = true;
    error_.clear();
    return true;
}

LottieCompositionDescriptor LottieAnimation::get_composition_info() const {
    LottieCompositionDescriptor desc;
    desc.width = width_;
    desc.height = height_;
    desc.start_frame = start_frame_;
    desc.end_frame = end_frame_;
    desc.frame_rate = frame_rate_;
    for (const auto& m : markers_) {
        desc.marker_names.push_back(m.name);
    }
    return desc;
}

std::vector<LottieMarker> LottieAnimation::get_markers() const {
    return markers_;
}

void LottieAnimation::play() {
    playing_ = true;
}

void LottieAnimation::play_segment(float start_frame, float end_frame) {
    segment_start_ = start_frame;
    segment_end_ = end_frame;
    use_segment_ = true;
    current_frame_ = start_frame;
    playing_ = true;
}

bool LottieAnimation::play_marker(const std::string& marker_name) {
    for (const auto& m : markers_) {
        if (m.name == marker_name) {
            play_segment(m.start_frame, m.end_frame);
            return true;
        }
    }
    return false;
}

void LottieAnimation::pause() {
    playing_ = false;
}

void LottieAnimation::stop() {
    playing_ = false;
    current_frame_ = use_segment_ ? segment_start_ : start_frame_;
}

void LottieAnimation::set_frame(float frame) {
    current_frame_ = frame;
}

float LottieAnimation::progress() const {
    float start = use_segment_ ? segment_start_ : start_frame_;
    float end   = use_segment_ ? segment_end_ : end_frame_;
    float range = end - start;
    if (range <= 0.0f) return 0.0f;
    return (current_frame_ - start) / range;
}

void LottieAnimation::advance(float delta_time) {
    if (!playing_ || !loaded_) return;

    float frames_advance = delta_time * frame_rate_ * speed_ * static_cast<float>(direction_);
    current_frame_ += frames_advance;

    float start = use_segment_ ? segment_start_ : start_frame_;
    float end   = use_segment_ ? segment_end_ : end_frame_;

    if (direction_ > 0 && current_frame_ >= end) {
        if (loop_) {
            current_frame_ = start + std::fmod(current_frame_ - start, end - start);
        } else {
            current_frame_ = end;
            playing_ = false;
        }
    } else if (direction_ < 0 && current_frame_ <= start) {
        if (loop_) {
            current_frame_ = end - std::fmod(start - current_frame_, end - start);
        } else {
            current_frame_ = start;
            playing_ = false;
        }
    }
}

void LottieAnimation::render_to_buffer(uint8_t* buffer, uint32_t width, uint32_t height) {
    if (!buffer || width == 0 || height == 0 || !loaded_) return;

    // Clear buffer
    std::memset(buffer, 0, width * height * 4);

    // In a production build, this would use rlottie or similar library
    // to render the current frame to the pixel buffer.
    // The API surface is designed to be compatible with:
    //   - rlottie (Samsung's Lottie renderer)
    //   - thorvg (Lottie + vector graphics renderer)
    //   - skottie (Skia's Lottie renderer)
}

} // namespace pictor
