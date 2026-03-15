#include "pictor/animation/ik_solver.h"
#include <cmath>
#include <algorithm>

namespace pictor {

float3 IKSolver::get_bone_position(const float4x4& world_matrix) {
    return world_matrix.get_translation();
}

float IKSolver::distance(const float3& a, const float3& b) {
    float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float3 IKSolver::normalize(const float3& v) {
    float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len < 1e-8f) return {0.0f, 1.0f, 0.0f};
    float inv = 1.0f / len;
    return {v.x * inv, v.y * inv, v.z * inv};
}

void IKSolver::solve(const Skeleton& skeleton,
                     const IKChainDescriptor& chain,
                     Transform* local_transforms,
                     const float4x4* world_matrices) {
    if (chain.weight <= 0.0f) return;

    switch (chain.solver_type) {
        case IKSolverType::TWO_BONE:
            solve_two_bone(skeleton, chain, local_transforms, world_matrices);
            break;
        case IKSolverType::CCD:
            solve_ccd(skeleton, chain, local_transforms, world_matrices);
            break;
        case IKSolverType::FABRIK:
            solve_fabrik(skeleton, chain, local_transforms, world_matrices);
            break;
    }
}

void IKSolver::apply_fk_overrides(const std::vector<FKPoseOverride>& overrides,
                                   Transform* local_transforms) {
    for (const auto& fk : overrides) {
        if (fk.blend_weight <= 0.0f) continue;

        Transform& t = local_transforms[fk.bone_index];
        if (fk.blend_weight >= 1.0f) {
            t = fk.local_transform;
        } else {
            t = lerp_transform(t, fk.local_transform, fk.blend_weight);
        }
    }
}

void IKSolver::solve_two_bone(const Skeleton& skeleton,
                               const IKChainDescriptor& chain,
                               Transform* local_transforms,
                               const float4x4* world_matrices) {
    // Two-bone IK: solve analytically for upper/lower bone angles
    // Chain: root_bone -> mid_bone -> end_effector
    uint32_t end_idx = chain.end_effector_bone;
    if (end_idx >= skeleton.bone_count()) return;

    int32_t mid_parent = skeleton.bone(end_idx).parent_index;
    if (mid_parent < 0) return;
    uint32_t mid_idx = static_cast<uint32_t>(mid_parent);

    int32_t root_parent = skeleton.bone(mid_idx).parent_index;
    if (root_parent < 0) return;
    uint32_t root_idx = static_cast<uint32_t>(root_parent);

    float3 root_pos = get_bone_position(world_matrices[root_idx]);
    float3 mid_pos  = get_bone_position(world_matrices[mid_idx]);
    float3 end_pos  = get_bone_position(world_matrices[end_idx]);
    float3 target   = chain.target_position;

    // Blend target with current position
    if (chain.weight < 1.0f) {
        target = {
            end_pos.x + chain.weight * (target.x - end_pos.x),
            end_pos.y + chain.weight * (target.y - end_pos.y),
            end_pos.z + chain.weight * (target.z - end_pos.z)
        };
    }

    float len_upper = distance(root_pos, mid_pos);
    float len_lower = distance(mid_pos, end_pos);
    float len_target = distance(root_pos, target);

    // Clamp target distance to reachable range
    float max_reach = len_upper + len_lower - 0.001f;
    float min_reach = std::abs(len_upper - len_lower) + 0.001f;
    len_target = std::clamp(len_target, min_reach, max_reach);

    // Law of cosines for mid joint angle
    float cos_mid = (len_upper * len_upper + len_lower * len_lower - len_target * len_target)
                    / (2.0f * len_upper * len_lower);
    cos_mid = std::clamp(cos_mid, -1.0f, 1.0f);
    float mid_angle = std::acos(cos_mid);

    // Direction from root to target
    float3 to_target = normalize(target - root_pos);

    // Angle at root joint
    float cos_root = (len_upper * len_upper + len_target * len_target - len_lower * len_lower)
                     / (2.0f * len_upper * len_target);
    cos_root = std::clamp(cos_root, -1.0f, 1.0f);
    float root_angle = std::acos(cos_root);

    // Compute rotation axis using pole vector
    float3 chain_dir = normalize(end_pos - root_pos);
    float3 pole_dir;
    if (chain.use_pole_vector) {
        pole_dir = normalize(chain.pole_vector - root_pos);
    } else {
        // Default pole: use the current mid position offset
        float3 mid_offset = mid_pos - root_pos;
        float3 proj = chain_dir * (mid_offset.x * chain_dir.x +
                                    mid_offset.y * chain_dir.y +
                                    mid_offset.z * chain_dir.z);
        pole_dir = normalize(float3{mid_offset.x - proj.x,
                                     mid_offset.y - proj.y,
                                     mid_offset.z - proj.z});
    }

    // Apply computed rotations to local transforms
    // Root bone: rotate to point toward target with root_angle offset
    float3 rot_axis = normalize(float3{
        to_target.y * pole_dir.z - to_target.z * pole_dir.y,
        to_target.z * pole_dir.x - to_target.x * pole_dir.z,
        to_target.x * pole_dir.y - to_target.y * pole_dir.x
    });

    local_transforms[root_idx].rotation =
        Quaternion::from_axis_angle(rot_axis, root_angle) *
        Quaternion::from_axis_angle({0, 0, 1}, 0); // Base orientation

    // Mid bone: apply bend angle
    local_transforms[mid_idx].rotation =
        Quaternion::from_axis_angle(rot_axis, 3.14159265f - mid_angle);
}

void IKSolver::solve_ccd(const Skeleton& skeleton,
                          const IKChainDescriptor& chain,
                          Transform* local_transforms,
                          const float4x4* world_matrices) {
    // Collect chain bone indices (from end effector back to root)
    std::vector<uint32_t> chain_bones;
    uint32_t current = chain.end_effector_bone;
    for (uint32_t i = 0; i < chain.chain_length && current < skeleton.bone_count(); ++i) {
        chain_bones.push_back(current);
        int32_t parent = skeleton.bone(current).parent_index;
        if (parent < 0) break;
        current = static_cast<uint32_t>(parent);
    }

    if (chain_bones.size() < 2) return;

    // Temporary world matrices for iterative update
    std::vector<float4x4> temp_world(skeleton.bone_count());
    skeleton.compute_world_matrices(local_transforms, temp_world.data());

    for (uint32_t iter = 0; iter < chain.max_iterations; ++iter) {
        float3 end_pos = get_bone_position(temp_world[chain.end_effector_bone]);
        float3 target = chain.target_position;

        if (chain.weight < 1.0f) {
            target = {
                end_pos.x + chain.weight * (target.x - end_pos.x),
                end_pos.y + chain.weight * (target.y - end_pos.y),
                end_pos.z + chain.weight * (target.z - end_pos.z)
            };
        }

        if (distance(end_pos, target) < chain.tolerance) break;

        // Iterate through chain from end toward root (skip end effector)
        for (uint32_t i = 1; i < chain_bones.size(); ++i) {
            uint32_t bone_idx = chain_bones[i];
            float3 bone_pos = get_bone_position(temp_world[bone_idx]);
            end_pos = get_bone_position(temp_world[chain.end_effector_bone]);

            float3 to_end = normalize(end_pos - bone_pos);
            float3 to_target = normalize(target - bone_pos);

            // Compute rotation from to_end to to_target
            float dot = to_end.x * to_target.x + to_end.y * to_target.y + to_end.z * to_target.z;
            dot = std::clamp(dot, -1.0f, 1.0f);
            float angle = std::acos(dot);

            if (angle > 1e-6f) {
                float3 axis = normalize(float3{
                    to_end.y * to_target.z - to_end.z * to_target.y,
                    to_end.z * to_target.x - to_end.x * to_target.z,
                    to_end.x * to_target.y - to_end.y * to_target.x
                });

                Quaternion rot = Quaternion::from_axis_angle(axis, angle);
                local_transforms[bone_idx].rotation = rot * local_transforms[bone_idx].rotation;
            }

            // Recompute world matrices after modification
            skeleton.compute_world_matrices(local_transforms, temp_world.data());
        }
    }
}

void IKSolver::solve_fabrik(const Skeleton& skeleton,
                             const IKChainDescriptor& chain,
                             Transform* local_transforms,
                             const float4x4* world_matrices) {
    // Collect chain bone indices
    std::vector<uint32_t> chain_bones;
    uint32_t current = chain.end_effector_bone;
    for (uint32_t i = 0; i <= chain.chain_length && current < skeleton.bone_count(); ++i) {
        chain_bones.push_back(current);
        int32_t parent = skeleton.bone(current).parent_index;
        if (parent < 0) break;
        current = static_cast<uint32_t>(parent);
    }

    if (chain_bones.size() < 2) return;

    // Reverse so chain_bones[0] = root of chain, last = end effector
    std::reverse(chain_bones.begin(), chain_bones.end());

    uint32_t n = static_cast<uint32_t>(chain_bones.size());

    // Extract current world positions
    std::vector<float3> positions(n);
    for (uint32_t i = 0; i < n; ++i) {
        positions[i] = get_bone_position(world_matrices[chain_bones[i]]);
    }

    // Compute bone lengths
    std::vector<float> lengths(n - 1);
    for (uint32_t i = 0; i < n - 1; ++i) {
        lengths[i] = distance(positions[i], positions[i + 1]);
    }

    float3 target = chain.target_position;
    float3 root_pos = positions[0];

    // Blend target
    if (chain.weight < 1.0f) {
        float3 end_pos = positions[n - 1];
        target = {
            end_pos.x + chain.weight * (target.x - end_pos.x),
            end_pos.y + chain.weight * (target.y - end_pos.y),
            end_pos.z + chain.weight * (target.z - end_pos.z)
        };
    }

    for (uint32_t iter = 0; iter < chain.max_iterations; ++iter) {
        if (distance(positions[n - 1], target) < chain.tolerance) break;

        // Forward pass: pull end effector to target
        positions[n - 1] = target;
        for (int i = static_cast<int>(n) - 2; i >= 0; --i) {
            float3 dir = normalize(positions[i] - positions[i + 1]);
            positions[i] = {
                positions[i + 1].x + dir.x * lengths[i],
                positions[i + 1].y + dir.y * lengths[i],
                positions[i + 1].z + dir.z * lengths[i]
            };
        }

        // Backward pass: pin root to original position
        positions[0] = root_pos;
        for (uint32_t i = 0; i < n - 1; ++i) {
            float3 dir = normalize(positions[i + 1] - positions[i]);
            positions[i + 1] = {
                positions[i].x + dir.x * lengths[i],
                positions[i].y + dir.y * lengths[i],
                positions[i].z + dir.z * lengths[i]
            };
        }
    }

    // Convert solved positions back to local transforms
    std::vector<float4x4> temp_world(skeleton.bone_count());
    skeleton.compute_world_matrices(local_transforms, temp_world.data());

    for (uint32_t i = 0; i < n - 1; ++i) {
        uint32_t bone_idx = chain_bones[i];
        float3 bone_pos = get_bone_position(temp_world[bone_idx]);

        // Current direction to child
        float3 child_pos = get_bone_position(temp_world[chain_bones[i + 1]]);
        float3 current_dir = normalize(child_pos - bone_pos);

        // Desired direction
        float3 desired_dir = normalize(positions[i + 1] - positions[i]);

        // Compute rotation from current to desired
        float dot = current_dir.x * desired_dir.x +
                    current_dir.y * desired_dir.y +
                    current_dir.z * desired_dir.z;
        dot = std::clamp(dot, -1.0f, 1.0f);
        float angle = std::acos(dot);

        if (angle > 1e-6f) {
            float3 axis = normalize(float3{
                current_dir.y * desired_dir.z - current_dir.z * desired_dir.y,
                current_dir.z * desired_dir.x - current_dir.x * desired_dir.z,
                current_dir.x * desired_dir.y - current_dir.y * desired_dir.x
            });
            Quaternion rot = Quaternion::from_axis_angle(axis, angle);
            local_transforms[bone_idx].rotation = rot * local_transforms[bone_idx].rotation;
        }

        // Recompute for next bone
        skeleton.compute_world_matrices(local_transforms, temp_world.data());
    }
}

} // namespace pictor
