#pragma once

#include "pictor/animation/animation_types.h"
#include <string>
#include <vector>
#include <memory>

namespace pictor {

/// Rive animation instance — manages a single Rive file (.riv).
/// Supports artboard selection, animation playback, and state machine interaction.
/// Renders to a pixel buffer that can be uploaded as a texture for GPU display.
class RiveAnimation {
public:
    RiveAnimation();
    ~RiveAnimation();

    RiveAnimation(const RiveAnimation&) = delete;
    RiveAnimation& operator=(const RiveAnimation&) = delete;

    /// Load a .riv file from disk
    bool load_file(const std::string& path);

    /// Load a .riv file from memory
    bool load_memory(const uint8_t* data, size_t size);

    /// Select an artboard by name (empty = default artboard)
    bool select_artboard(const std::string& name = "");

    /// Get information about the current artboard
    RiveArtboardDescriptor get_artboard_info() const;

    // ---- Simple Animation Playback ----

    /// Play a named animation
    bool play_animation(const std::string& name);

    /// Stop the current animation
    void stop_animation();

    /// Check if an animation is playing
    bool is_animation_playing() const { return animation_playing_; }

    // ---- State Machine ----

    /// Activate a state machine by name
    bool activate_state_machine(const std::string& name);

    /// Set a boolean input on the state machine
    void set_input_bool(const std::string& name, bool value);

    /// Set a number input on the state machine
    void set_input_number(const std::string& name, float value);

    /// Fire a trigger input on the state machine
    void fire_input_trigger(const std::string& name);

    // ---- Update & Render ----

    /// Advance the animation by delta_time seconds
    void advance(float delta_time);

    /// Render to a pixel buffer (RGBA8).
    /// @param buffer   Output pixel buffer (must be width * height * 4 bytes)
    /// @param width    Render width in pixels
    /// @param height   Render height in pixels
    void render_to_buffer(uint8_t* buffer, uint32_t width, uint32_t height);

    /// Get the natural width/height of the artboard
    float artboard_width() const { return artboard_width_; }
    float artboard_height() const { return artboard_height_; }

    bool is_loaded() const { return loaded_; }
    const std::string& error() const { return error_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool        loaded_            = false;
    bool        animation_playing_ = false;
    float       artboard_width_    = 0.0f;
    float       artboard_height_   = 0.0f;
    std::string error_;

    // Raw file data kept for artboard re-selection
    std::vector<uint8_t> file_data_;
};

} // namespace pictor
