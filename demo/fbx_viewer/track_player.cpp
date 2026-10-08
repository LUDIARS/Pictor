#include "track_player.h"

#include "track_rotation.h"

namespace pictor_fbx_viewer {

void TrackPlayer::initialize(TrackData track, const pictor::SkeletonDescriptor& skeleton,
                             const MorphTargets* morphs) {
    track_  = std::move(track);
    morphs_ = morphs;
    bind_local_.clear();
    bind_local_.reserve(track_.bone_index.size());
    for (uint32_t bone : track_.bone_index)
        bind_local_.push_back(bone < skeleton.bones.size() ? skeleton.bones[bone].bind_pose
                                                           : pictor::Transform{});
    weights_.assign(morphs_ ? morphs_->channel_count() : 0, 0.0f);
    applier_.bind(morphs_);
}

/// @implements SPEC-PC-FBX-TRACK-PLAYBACK
void TrackPlayer::apply_bones(pictor::AnimationSystem& anim, pictor::AnimationStateHandle handle,
                              uint32_t frame) const {
    if (frame >= track_.frame_count) return;
    const float* euler = track_.bone_frame(frame);
    for (uint32_t g = 0; g < track_.bone_group_count(); ++g) {
        const float* e = euler + static_cast<size_t>(g) * 3;
        anim.set_fk_override(handle, track_.bone_index[g],
                             track_local_transform(bind_local_[g], e[0], e[1], e[2]), 1.0f);
    }
}

/// @implements SPEC-PC-FBX-TRACK-PLAYBACK
void TrackPlayer::apply_morphs(uint32_t frame, float* positions, size_t stride_floats) {
    if (!has_morphs() || frame >= track_.frame_count) return;
    const float* w = track_.morph_frame(frame);
    for (uint32_t m = 0; m < track_.morph_count(); ++m) {
        const uint32_t channel = track_.morph_index[m];
        if (channel < weights_.size()) weights_[channel] = w[m];
    }
    applier_.apply(weights_.data(), positions, stride_floats);
}

} // namespace pictor_fbx_viewer
