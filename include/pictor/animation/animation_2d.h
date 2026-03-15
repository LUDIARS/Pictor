#pragma once

#include "pictor/animation/animation_types.h"
#include <vector>
#include <string>

namespace pictor {

/// 2D animation channel — animates a single property of a 2D element
struct Animation2DChannel {
    uint32_t         target_index = 0;  // Target element index
    Channel2DTarget  target       = Channel2DTarget::POSITION_X;
    InterpolationMode interpolation = InterpolationMode::LINEAR;
    std::vector<Keyframe> keyframes;
};

/// 2D animation clip — a complete 2D animation with multiple channels
struct Animation2DClipDescriptor {
    std::string name;
    float       duration    = 0.0f;
    float       sample_rate = 30.0f;
    WrapMode    wrap_mode   = WrapMode::LOOP;
    std::vector<Animation2DChannel> channels;
};

/// Sprite animation clip — frame-by-frame sprite sheet animation
struct SpriteAnimationDescriptor {
    std::string name;
    TextureHandle sprite_sheet = INVALID_TEXTURE;
    uint32_t    sheet_columns = 1;
    uint32_t    sheet_rows    = 1;
    float       frame_rate    = 12.0f;
    WrapMode    wrap_mode     = WrapMode::LOOP;
    std::vector<SpriteFrame> frames;
};

/// 2D animation player — evaluates 2D animation clips
class Animation2DPlayer {
public:
    Animation2DPlayer() = default;
    ~Animation2DPlayer() = default;

    /// Load a 2D transform animation clip
    void load_clip(const Animation2DClipDescriptor& desc);

    /// Load a sprite animation clip
    void load_sprite_clip(const SpriteAnimationDescriptor& desc);

    /// Evaluate the current 2D animation at the given time.
    /// @param time           Current playback time
    /// @param out_transforms Output 2D transforms per element
    /// @param max_elements   Size of output array
    void evaluate(float time, Transform2D* out_transforms, uint32_t max_elements) const;

    /// Evaluate sprite animation at the given time.
    /// @param time  Current playback time
    /// @return The active sprite frame
    SpriteFrame evaluate_sprite(float time) const;

    /// Advance playback time
    void advance(float delta_time);

    /// Control playback
    void play()  { playing_ = true; }
    void pause() { playing_ = false; }
    void stop()  { playing_ = false; current_time_ = 0.0f; }
    void set_time(float time) { current_time_ = time; }
    void set_speed(float speed) { speed_ = speed; }
    void set_wrap_mode(WrapMode mode) { wrap_mode_ = mode; }

    bool is_playing() const { return playing_; }
    float current_time() const { return current_time_; }
    float duration() const { return duration_; }

private:
    float wrap_time(float time) const;

    std::vector<Animation2DChannel> channels_;
    std::vector<SpriteFrame>        sprite_frames_;
    std::string name_;
    float       duration_     = 0.0f;
    float       current_time_ = 0.0f;
    float       speed_        = 1.0f;
    float       frame_rate_   = 12.0f;
    WrapMode    wrap_mode_    = WrapMode::LOOP;
    bool        playing_      = false;
    bool        is_sprite_    = false;
};

} // namespace pictor
