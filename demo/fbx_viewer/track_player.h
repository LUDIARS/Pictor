// Per-frame driver for `--track` (SPEC-PC-FBX-TRACK-PLAYBACK): frame `i`
// of the parsed track sets the FK overrides of the targeted bones
// (`bind_local * R(rx, ry, rz)`) and writes the blended shape positions.
// Everything is sized in initialize(); per-frame calls do not allocate.
#pragma once

#include "morph_targets.h"
#include "track_csv.h"

#include "pictor/animation/animation_system.h"

#include <vector>

namespace pictor_fbx_viewer {

class TrackPlayer {
public:
    /// `morphs` must outlive the player; track morph channels index its names.
    void initialize(TrackData track, const pictor::SkeletonDescriptor& skeleton,
                    const MorphTargets* morphs);

    bool     active() const { return track_.frame_count > 0; }
    uint32_t frame_count() const { return track_.frame_count; }
    bool     has_morphs() const { return track_.morph_count() > 0 && morphs_ && !morphs_->empty(); }

    void apply_bones(pictor::AnimationSystem& anim, pictor::AnimationStateHandle handle,
                     uint32_t frame) const;
    /// Writes into a strided position array (the mapped vertex buffer).
    void apply_morphs(uint32_t frame, float* positions, size_t stride_floats);

private:
    TrackData                      track_;
    std::vector<pictor::Transform> bind_local_;  // per bone group
    const MorphTargets*            morphs_ = nullptr;
    std::vector<float>             weights_;     // per MorphTargets channel
    MorphApplier                   applier_;
};

} // namespace pictor_fbx_viewer
