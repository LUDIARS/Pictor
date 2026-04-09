#include "pictor/animation/animation_2d.h"
#include <cmath>
#include <algorithm>

namespace pictor {

void Animation2DPlayer::load_clip(const Animation2DClipDescriptor& desc) {
    name_ = desc.name;
    duration_ = desc.duration;
    wrap_mode_ = desc.wrap_mode;
    channels_ = desc.channels;
    is_sprite_ = false;
    current_time_ = 0.0f;
}

void Animation2DPlayer::load_sprite_clip(const SpriteAnimationDescriptor& desc) {
    name_ = desc.name;
    frame_rate_ = desc.frame_rate;
    wrap_mode_ = desc.wrap_mode;
    sprite_frames_ = desc.frames;
    is_sprite_ = true;
    current_time_ = 0.0f;

    if (!sprite_frames_.empty()) {
        duration_ = sprite_frames_.back().time;
        if (duration_ <= 0.0f && frame_rate_ > 0.0f) {
            duration_ = static_cast<float>(sprite_frames_.size()) / frame_rate_;
            // Assign times if not set
            for (size_t i = 0; i < sprite_frames_.size(); ++i) {
                sprite_frames_[i].time = static_cast<float>(i) / frame_rate_;
            }
        }
    }
}

float Animation2DPlayer::wrap_time(float time) const {
    if (duration_ <= 0.0f) return 0.0f;

    switch (wrap_mode_) {
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

void Animation2DPlayer::advance(float delta_time) {
    if (!playing_) return;
    current_time_ += delta_time * speed_;
}

void Animation2DPlayer::evaluate(float time, Transform2D* out_transforms, uint32_t max_elements) const {
    if (is_sprite_ || channels_.empty()) return;

    float wrapped = const_cast<Animation2DPlayer*>(this)->wrap_time(time);

    for (const auto& channel : channels_) {
        if (channel.target_index >= max_elements) continue;
        if (channel.keyframes.empty()) continue;

        Transform2D& t = out_transforms[channel.target_index];

        // Find keyframe pair
        float value = 0.0f;
        if (channel.keyframes.size() == 1) {
            value = channel.keyframes[0].value[0];
        } else {
            // Binary search for keyframe pair
            uint32_t lo = 0, hi = static_cast<uint32_t>(channel.keyframes.size() - 1);
            while (lo < hi - 1) {
                uint32_t mid = (lo + hi) / 2;
                if (channel.keyframes[mid].time <= wrapped) lo = mid;
                else hi = mid;
            }

            const auto& ka = channel.keyframes[lo];
            const auto& kb = channel.keyframes[hi];

            float span = kb.time - ka.time;
            float frac = (span > 0.0f) ? (wrapped - ka.time) / span : 0.0f;

            switch (channel.interpolation) {
                case InterpolationMode::STEP:
                    value = ka.value[0];
                    break;
                case InterpolationMode::LINEAR:
                    value = ka.value[0] + frac * (kb.value[0] - ka.value[0]);
                    break;
                case InterpolationMode::CUBIC: {
                    float t2 = frac * frac;
                    float t3 = t2 * frac;
                    value = (2.0f * t3 - 3.0f * t2 + 1.0f) * ka.value[0] +
                            (t3 - 2.0f * t2 + frac) * ka.out_tangent[0] * span +
                            (-2.0f * t3 + 3.0f * t2) * kb.value[0] +
                            (t3 - t2) * kb.in_tangent[0] * span;
                    break;
                }
            }
        }

        switch (channel.target) {
            case Channel2DTarget::POSITION_X: t.x = value; break;
            case Channel2DTarget::POSITION_Y: t.y = value; break;
            case Channel2DTarget::ROTATION:   t.rotation = value; break;
            case Channel2DTarget::SCALE_X:    t.scale_x = value; break;
            case Channel2DTarget::SCALE_Y:    t.scale_y = value; break;
            case Channel2DTarget::OPACITY:    t.opacity = value; break;
            case Channel2DTarget::ANCHOR_X:   t.anchor_x = value; break;
            case Channel2DTarget::ANCHOR_Y:   t.anchor_y = value; break;
        }
    }
}

SpriteFrame Animation2DPlayer::evaluate_sprite(float time) const {
    if (sprite_frames_.empty()) return {};

    float wrapped = const_cast<Animation2DPlayer*>(this)->wrap_time(time);

    // Find the active frame (last frame with time <= wrapped)
    const SpriteFrame* result = &sprite_frames_[0];
    for (const auto& frame : sprite_frames_) {
        if (frame.time <= wrapped) {
            result = &frame;
        } else {
            break;
        }
    }
    return *result;
}

} // namespace pictor
