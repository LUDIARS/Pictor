#pragma once

#include "pictor/animation/animation_types.h"
#include <vector>
#include <string>

namespace pictor {

/// A single layer in a vector animation (group of animated paths)
struct VectorLayer {
    std::string               name;
    std::vector<VectorKeyframe> keyframes;
    InterpolationMode         interpolation = InterpolationMode::LINEAR;
    float                     opacity = 1.0f;
    bool                      visible = true;
};

/// Vector animation clip descriptor
struct VectorAnimationDescriptor {
    std::string              name;
    float                    width    = 0.0f;
    float                    height   = 0.0f;
    float                    duration = 0.0f;
    WrapMode                 wrap_mode = WrapMode::LOOP;
    std::vector<VectorLayer> layers;
};

/// Evaluated vector frame — result of evaluating a vector animation at a point in time
struct VectorFrame {
    std::vector<VectorPath> paths;
    std::vector<Transform2D> transforms;
    float time = 0.0f;
};

/// Vector animation player — evaluates vector animation clips over time.
/// Supports path morphing, transform animation, and multi-layer compositing.
class VectorAnimationPlayer {
public:
    VectorAnimationPlayer() = default;
    ~VectorAnimationPlayer() = default;

    /// Load a vector animation clip
    void load(const VectorAnimationDescriptor& desc);

    /// Load from SVG animation data (SMIL animated SVG)
    bool load_svg(const std::string& svg_data);

    /// Advance playback time
    void advance(float delta_time);

    /// Evaluate the current frame
    VectorFrame evaluate() const;

    /// Evaluate at a specific time
    VectorFrame evaluate_at(float time) const;

    /// Rasterize the current frame to a pixel buffer (RGBA8).
    /// @param buffer   Output buffer (width * height * 4 bytes)
    /// @param width    Output width
    /// @param height   Output height
    void rasterize(uint8_t* buffer, uint32_t width, uint32_t height) const;

    // ---- Playback Control ----

    void play()  { playing_ = true; }
    void pause() { playing_ = false; }
    void stop()  { playing_ = false; current_time_ = 0.0f; }
    void set_time(float time) { current_time_ = time; }
    void set_speed(float speed) { speed_ = speed; }
    void set_wrap_mode(WrapMode mode) { wrap_mode_ = mode; }

    bool  is_playing() const { return playing_; }
    float current_time() const { return current_time_; }
    float duration() const { return duration_; }
    float width() const { return width_; }
    float height() const { return height_; }

private:
    /// Interpolate between two vector keyframes
    VectorKeyframe interpolate_keyframes(const VectorKeyframe& a,
                                         const VectorKeyframe& b,
                                         float t) const;

    /// Interpolate between two vector paths (morphing)
    VectorPath interpolate_paths(const VectorPath& a,
                                 const VectorPath& b,
                                 float t) const;

    float wrap_time(float time) const;

    std::vector<VectorLayer> layers_;
    std::string name_;
    float       width_        = 0.0f;
    float       height_       = 0.0f;
    float       duration_     = 0.0f;
    float       current_time_ = 0.0f;
    float       speed_        = 1.0f;
    WrapMode    wrap_mode_    = WrapMode::LOOP;
    bool        playing_      = false;
};

} // namespace pictor
