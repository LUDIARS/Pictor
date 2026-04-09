#include "pictor/animation/animation_clip.h"
#include <cmath>
#include <algorithm>

namespace pictor {

AnimationClip::AnimationClip(const AnimationClipDescriptor& desc)
    : name_(desc.name)
    , duration_(desc.duration)
    , sample_rate_(desc.sample_rate)
    , wrap_mode_(desc.wrap_mode)
    , channels_(desc.channels)
{}

float AnimationClip::wrap_time(float time, WrapMode mode) const {
    if (duration_ <= 0.0f) return 0.0f;

    switch (mode) {
        case WrapMode::ONCE:
            return (time >= duration_) ? duration_ : time;

        case WrapMode::LOOP: {
            float t = std::fmod(time, duration_);
            return (t < 0.0f) ? t + duration_ : t;
        }

        case WrapMode::PING_PONG: {
            float t = std::fmod(time, duration_ * 2.0f);
            if (t < 0.0f) t += duration_ * 2.0f;
            return (t > duration_) ? (duration_ * 2.0f - t) : t;
        }

        case WrapMode::CLAMP:
            return std::clamp(time, 0.0f, duration_);
    }
    return time;
}

void AnimationClip::find_keyframe_pair(const std::vector<Keyframe>& keyframes,
                                       float time,
                                       uint32_t& out_index_a,
                                       uint32_t& out_index_b,
                                       float& out_t) {
    if (keyframes.empty()) {
        out_index_a = out_index_b = 0;
        out_t = 0.0f;
        return;
    }

    if (keyframes.size() == 1 || time <= keyframes.front().time) {
        out_index_a = out_index_b = 0;
        out_t = 0.0f;
        return;
    }

    if (time >= keyframes.back().time) {
        out_index_a = out_index_b = static_cast<uint32_t>(keyframes.size() - 1);
        out_t = 0.0f;
        return;
    }

    // Binary search for the surrounding keyframe pair
    uint32_t lo = 0;
    uint32_t hi = static_cast<uint32_t>(keyframes.size() - 1);
    while (lo < hi - 1) {
        uint32_t mid = (lo + hi) / 2;
        if (keyframes[mid].time <= time) {
            lo = mid;
        } else {
            hi = mid;
        }
    }

    out_index_a = lo;
    out_index_b = hi;

    float span = keyframes[hi].time - keyframes[lo].time;
    out_t = (span > 0.0f) ? (time - keyframes[lo].time) / span : 0.0f;
}

float AnimationClip::cubic_hermite(float p0, float p1, float m0, float m1, float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    return (2.0f * t3 - 3.0f * t2 + 1.0f) * p0 +
           (t3 - 2.0f * t2 + t) * m0 +
           (-2.0f * t3 + 3.0f * t2) * p1 +
           (t3 - t2) * m1;
}

void AnimationClip::evaluate_channel(const AnimationChannel& channel,
                                     float time, float out[4]) const {
    if (channel.keyframes.empty()) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }

    uint32_t ia, ib;
    float t;
    find_keyframe_pair(channel.keyframes, time, ia, ib, t);

    const auto& ka = channel.keyframes[ia];
    const auto& kb = channel.keyframes[ib];

    uint32_t components = (channel.target == ChannelTarget::ROTATION) ? 4 : 3;

    switch (channel.interpolation) {
        case InterpolationMode::STEP:
            for (uint32_t i = 0; i < components; ++i)
                out[i] = ka.value[i];
            break;

        case InterpolationMode::LINEAR:
            if (channel.target == ChannelTarget::ROTATION) {
                // SLERP for rotations
                Quaternion qa{ka.value[0], ka.value[1], ka.value[2], ka.value[3]};
                Quaternion qb{kb.value[0], kb.value[1], kb.value[2], kb.value[3]};
                Quaternion result = slerp(qa, qb, t);
                out[0] = result.x;
                out[1] = result.y;
                out[2] = result.z;
                out[3] = result.w;
            } else {
                for (uint32_t i = 0; i < components; ++i)
                    out[i] = ka.value[i] + t * (kb.value[i] - ka.value[i]);
            }
            break;

        case InterpolationMode::CUBIC:
            for (uint32_t i = 0; i < components; ++i) {
                float span = kb.time - ka.time;
                out[i] = cubic_hermite(
                    ka.value[i], kb.value[i],
                    ka.out_tangent[i] * span,
                    kb.in_tangent[i] * span, t);
            }
            if (channel.target == ChannelTarget::ROTATION) {
                // Normalize quaternion after cubic interpolation
                Quaternion q{out[0], out[1], out[2], out[3]};
                q = q.normalized();
                out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
            }
            break;
    }
}

void AnimationClip::evaluate(float time, Transform* out_transforms, uint32_t max_targets) const {
    float wrapped = wrap_time(time, wrap_mode_);

    for (const auto& channel : channels_) {
        if (channel.target_index >= max_targets) continue;

        float values[4] = {};
        evaluate_channel(channel, wrapped, values);

        Transform& t = out_transforms[channel.target_index];
        switch (channel.target) {
            case ChannelTarget::TRANSLATION:
                t.translation = {values[0], values[1], values[2]};
                break;
            case ChannelTarget::ROTATION:
                t.rotation = {values[0], values[1], values[2], values[3]};
                break;
            case ChannelTarget::SCALE:
                t.scale = {values[0], values[1], values[2]};
                break;
            case ChannelTarget::WEIGHTS:
                // Morph target weights — store in translation as temp
                t.translation = {values[0], values[1], values[2]};
                break;
        }
    }
}

} // namespace pictor
