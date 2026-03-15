#pragma once

#include "pictor/animation/animation_types.h"
#include <string>
#include <vector>
#include <memory>

namespace pictor {

/// Lottie animation instance — manages a single Lottie JSON file.
/// Supports playback control, segment play, and marker-based navigation.
/// Renders to a pixel buffer that can be uploaded as a texture.
class LottieAnimation {
public:
    LottieAnimation();
    ~LottieAnimation();

    LottieAnimation(const LottieAnimation&) = delete;
    LottieAnimation& operator=(const LottieAnimation&) = delete;

    /// Load a Lottie JSON file from disk
    bool load_file(const std::string& path);

    /// Load from a JSON string in memory
    bool load_json(const std::string& json_data);

    /// Get composition info
    LottieCompositionDescriptor get_composition_info() const;

    /// Get named markers
    std::vector<LottieMarker> get_markers() const;

    // ---- Playback Control ----

    /// Play the full animation
    void play();

    /// Play a specific frame range
    void play_segment(float start_frame, float end_frame);

    /// Play a named marker segment
    bool play_marker(const std::string& marker_name);

    /// Pause playback
    void pause();

    /// Stop and reset to start
    void stop();

    /// Set the current frame directly
    void set_frame(float frame);

    /// Set playback speed (1.0 = normal, -1.0 = reverse)
    void set_speed(float speed) { speed_ = speed; }

    /// Set loop mode
    void set_loop(bool loop) { loop_ = loop; }

    /// Set playback direction
    void set_direction(int direction) { direction_ = direction > 0 ? 1 : -1; }

    // ---- Update & Render ----

    /// Advance the animation by delta_time seconds
    void advance(float delta_time);

    /// Render the current frame to a pixel buffer (RGBA8).
    /// @param buffer   Output pixel buffer (must be width * height * 4 bytes)
    /// @param width    Render width in pixels
    /// @param height   Render height in pixels
    void render_to_buffer(uint8_t* buffer, uint32_t width, uint32_t height);

    // ---- State Queries ----

    bool  is_loaded() const { return loaded_; }
    bool  is_playing() const { return playing_; }
    float current_frame() const { return current_frame_; }
    float total_frames() const { return end_frame_ - start_frame_; }
    float frame_rate() const { return frame_rate_; }
    float duration() const { return total_frames() / frame_rate_; }
    float progress() const;  // 0.0 - 1.0
    float width() const { return width_; }
    float height() const { return height_; }
    const std::string& error() const { return error_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool        loaded_        = false;
    bool        playing_       = false;
    bool        loop_          = true;
    float       current_frame_ = 0.0f;
    float       start_frame_   = 0.0f;
    float       end_frame_     = 0.0f;
    float       frame_rate_    = 30.0f;
    float       speed_         = 1.0f;
    int         direction_     = 1;
    float       width_         = 0.0f;
    float       height_        = 0.0f;
    std::string error_;

    // Segment playback
    float segment_start_ = 0.0f;
    float segment_end_   = 0.0f;
    bool  use_segment_   = false;

    // Marker cache
    std::vector<LottieMarker> markers_;
};

} // namespace pictor
