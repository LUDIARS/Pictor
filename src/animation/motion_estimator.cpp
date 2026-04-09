#include "pictor/animation/motion_estimator.h"
#include <cmath>
#include <algorithm>

namespace pictor {

MotionDistanceResult MotionEstimator::estimate(const AnimationClip& clip,
                                                const Skeleton& skeleton,
                                                float sample_rate) const {
    if (clip.duration() <= 0.0f || skeleton.bone_count() == 0) {
        return {};
    }

    if (sample_rate <= 0.0f) {
        sample_rate = clip.sample_rate();
    }

    float dt = 1.0f / sample_rate;
    uint32_t num_samples = static_cast<uint32_t>(clip.duration() * sample_rate) + 1;
    num_samples = std::max(num_samples, 2u);

    // Evaluate root bone position at each sample
    std::vector<float3> positions(num_samples);
    uint32_t bone_count = skeleton.bone_count();

    std::vector<Transform> local_pose(bone_count);
    std::vector<float4x4> world_matrices(bone_count);

    for (uint32_t i = 0; i < num_samples; ++i) {
        float time = std::min(static_cast<float>(i) * dt, clip.duration());

        // Start with bind pose
        skeleton.get_bind_pose(local_pose.data());

        // Evaluate animation
        clip.evaluate(time, local_pose.data(), bone_count);

        // Compute world matrices
        skeleton.compute_world_matrices(local_pose.data(), world_matrices.data());

        // Root bone (index 0) world position
        positions[i] = world_matrices[0].get_translation();
    }

    return estimate_from_positions(positions.data(), num_samples, clip.duration());
}

MotionDistanceResult MotionEstimator::estimate_from_positions(const float3* positions,
                                                               uint32_t count,
                                                               float duration) const {
    MotionDistanceResult result;
    if (count < 2 || duration <= 0.0f) return result;

    float dt = duration / static_cast<float>(count - 1);
    result.speed_curve.resize(count - 1);

    float total_dist = 0.0f;
    float total_vert = 0.0f;
    float total_horiz = 0.0f;
    float peak_speed = 0.0f;

    for (uint32_t i = 1; i < count; ++i) {
        float dx = positions[i].x - positions[i - 1].x;
        float dy = positions[i].y - positions[i - 1].y;
        float dz = positions[i].z - positions[i - 1].z;

        float seg_dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        float vert_dist = std::abs(dy);
        float horiz_dist = std::sqrt(dx * dx + dz * dz);

        total_dist += seg_dist;
        total_vert += vert_dist;
        total_horiz += horiz_dist;

        float speed = seg_dist / dt;
        result.speed_curve[i - 1] = speed;
        peak_speed = std::max(peak_speed, speed);
    }

    result.total_distance = total_dist;
    result.average_speed = total_dist / duration;
    result.peak_speed = peak_speed;
    result.vertical_distance = total_vert;
    result.horizontal_distance = total_horiz;
    result.net_displacement = {
        positions[count - 1].x - positions[0].x,
        positions[count - 1].y - positions[0].y,
        positions[count - 1].z - positions[0].z
    };

    return result;
}

MotionDistanceResult MotionEstimator::estimate_2d(const float* positions_x,
                                                   const float* positions_y,
                                                   uint32_t count,
                                                   float duration) const {
    if (count < 2 || duration <= 0.0f) return {};

    std::vector<float3> positions(count);
    for (uint32_t i = 0; i < count; ++i) {
        positions[i] = {positions_x[i], positions_y[i], 0.0f};
    }
    return estimate_from_positions(positions.data(), count, duration);
}

float MotionEstimator::estimate_foot_speed(const AnimationClip& clip,
                                            const Skeleton& skeleton,
                                            const std::vector<uint32_t>& foot_bone_indices) const {
    if (clip.duration() <= 0.0f || skeleton.bone_count() == 0 || foot_bone_indices.empty()) {
        return 0.0f;
    }

    float sample_rate = clip.sample_rate();
    float dt = 1.0f / sample_rate;
    uint32_t num_samples = static_cast<uint32_t>(clip.duration() * sample_rate) + 1;
    num_samples = std::max(num_samples, 2u);

    uint32_t bone_count = skeleton.bone_count();
    std::vector<Transform> local_pose(bone_count);
    std::vector<float4x4> world_matrices(bone_count);

    float total_speed = 0.0f;
    uint32_t speed_samples = 0;

    // Previous frame foot positions
    std::vector<float3> prev_foot_pos(foot_bone_indices.size());

    for (uint32_t s = 0; s < num_samples; ++s) {
        float time = std::min(static_cast<float>(s) * dt, clip.duration());

        skeleton.get_bind_pose(local_pose.data());
        clip.evaluate(time, local_pose.data(), bone_count);
        skeleton.compute_world_matrices(local_pose.data(), world_matrices.data());

        for (size_t f = 0; f < foot_bone_indices.size(); ++f) {
            uint32_t bone_idx = foot_bone_indices[f];
            if (bone_idx >= bone_count) continue;

            float3 pos = world_matrices[bone_idx].get_translation();

            if (s > 0) {
                float dx = pos.x - prev_foot_pos[f].x;
                float dy = pos.y - prev_foot_pos[f].y;
                float dz = pos.z - prev_foot_pos[f].z;
                float speed = std::sqrt(dx * dx + dy * dy + dz * dz) / dt;
                total_speed += speed;
                ++speed_samples;
            }

            prev_foot_pos[f] = pos;
        }
    }

    return (speed_samples > 0) ? total_speed / static_cast<float>(speed_samples) : 0.0f;
}

} // namespace pictor
