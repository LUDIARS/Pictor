#pragma once

#include "pictor/animation/animation_types.h"
#include <algorithm>

namespace pictor {

/// Manages a single animation clip with keyframe evaluation and interpolation.
/// Supports translation, rotation, scale, and morph weight channels.
class AnimationClip {
public:
    AnimationClip() = default;
    explicit AnimationClip(const AnimationClipDescriptor& desc);
    ~AnimationClip() = default;

    /// Evaluate all channels at the given time, writing results to output arrays.
    /// @param time        Current playback time (seconds)
    /// @param out_transforms  Output local transforms per target index (must be pre-allocated)
    /// @param max_targets     Size of out_transforms array
    void evaluate(float time, Transform* out_transforms, uint32_t max_targets) const;

    /// Evaluate a single channel at the given time
    void evaluate_channel(const AnimationChannel& channel, float time, float out[4]) const;

    /// Get the looped/clamped time based on wrap mode
    float wrap_time(float time, WrapMode mode) const;

    const std::string& name() const { return name_; }
    float duration() const { return duration_; }
    float sample_rate() const { return sample_rate_; }
    WrapMode wrap_mode() const { return wrap_mode_; }
    const std::vector<AnimationChannel>& channels() const { return channels_; }

    void set_wrap_mode(WrapMode mode) { wrap_mode_ = mode; }

private:
    /// Find the keyframe pair surrounding the given time
    static void find_keyframe_pair(const std::vector<Keyframe>& keyframes,
                                   float time,
                                   uint32_t& out_index_a,
                                   uint32_t& out_index_b,
                                   float& out_t);

    /// Cubic Hermite interpolation
    static float cubic_hermite(float p0, float p1, float m0, float m1, float t);

    std::string name_;
    float       duration_    = 0.0f;
    float       sample_rate_ = 30.0f;
    WrapMode    wrap_mode_   = WrapMode::LOOP;
    std::vector<AnimationChannel> channels_;
};

} // namespace pictor
