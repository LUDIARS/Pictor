#pragma once

#include "pictor/animation/animation_types.h"
#include "pictor/animation/animation_clip.h"
#include "pictor/animation/skeleton.h"

namespace pictor {

/// Estimates motion distance and speed from animation clips.
/// Analyzes root bone movement to compute travel distance, speed curves,
/// and displacement vectors useful for locomotion systems.
class MotionEstimator {
public:
    MotionEstimator() = default;
    ~MotionEstimator() = default;

    /// Estimate motion distance from a skeletal animation clip.
    /// Samples the root bone trajectory at the given sample rate.
    /// @param clip       The animation clip to analyze
    /// @param skeleton   The skeleton (uses root bone for motion)
    /// @param sample_rate  Samples per second for estimation (default: clip sample rate)
    /// @return  Motion distance analysis result
    MotionDistanceResult estimate(const AnimationClip& clip,
                                  const Skeleton& skeleton,
                                  float sample_rate = 0.0f) const;

    /// Estimate motion distance from raw position keyframes (no skeleton needed).
    /// Useful for object-level motion or 2D animations.
    /// @param positions   Array of position samples
    /// @param count       Number of samples
    /// @param duration    Total duration in seconds
    /// @return  Motion distance analysis result
    MotionDistanceResult estimate_from_positions(const float3* positions,
                                                 uint32_t count,
                                                 float duration) const;

    /// Estimate motion from 2D position keyframes.
    /// @param positions_x  X position samples
    /// @param positions_y  Y position samples
    /// @param count        Number of samples
    /// @param duration     Total duration in seconds
    /// @return  Motion distance analysis result (z components are zero)
    MotionDistanceResult estimate_2d(const float* positions_x,
                                     const float* positions_y,
                                     uint32_t count,
                                     float duration) const;

    /// Compute the average foot speed for locomotion matching.
    /// Analyzes multiple foot bones to estimate ground speed.
    /// @param clip       The animation clip
    /// @param skeleton   The skeleton
    /// @param foot_bone_indices  Indices of foot bones to analyze
    /// @return Average foot-contact speed (units/sec)
    float estimate_foot_speed(const AnimationClip& clip,
                              const Skeleton& skeleton,
                              const std::vector<uint32_t>& foot_bone_indices) const;
};

} // namespace pictor
