#include "pictor/animation/skeleton.h"

namespace pictor {

Skeleton::Skeleton(const SkeletonDescriptor& desc)
    : name_(desc.name)
    , bones_(desc.bones)
{
    for (int32_t i = 0; i < static_cast<int32_t>(bones_.size()); ++i) {
        bone_name_map_[bones_[i].name] = i;
    }
}

int32_t Skeleton::find_bone(const std::string& name) const {
    auto it = bone_name_map_.find(name);
    return (it != bone_name_map_.end()) ? it->second : -1;
}

void Skeleton::get_bind_pose(Transform* out_transforms) const {
    for (uint32_t i = 0; i < bones_.size(); ++i) {
        out_transforms[i] = bones_[i].bind_pose;
    }
}

void Skeleton::compute_world_matrices(const Transform* local_transforms,
                                      float4x4* out_world_matrices) const {
    for (uint32_t i = 0; i < bones_.size(); ++i) {
        float4x4 local_matrix = local_transforms[i].to_matrix();

        if (bones_[i].parent_index < 0) {
            // Root bone — local is world
            out_world_matrices[i] = local_matrix;
        } else {
            // Child bone — multiply with parent's world matrix
            const float4x4& parent = out_world_matrices[bones_[i].parent_index];
            float4x4& result = out_world_matrices[i];

            // Matrix multiplication: result = parent * local
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    result.m[r][c] = 0.0f;
                    for (int k = 0; k < 4; ++k) {
                        result.m[r][c] += parent.m[r][k] * local_matrix.m[k][c];
                    }
                }
            }
        }
    }
}

void Skeleton::compute_skinning_matrices(const Transform* local_transforms,
                                         float4x4* out_skinning_matrices) const {
    // First compute world matrices
    std::vector<float4x4> world_matrices(bones_.size());
    compute_world_matrices(local_transforms, world_matrices.data());

    // Then multiply by inverse bind matrix: skinning = world * inverse_bind
    for (uint32_t i = 0; i < bones_.size(); ++i) {
        const float4x4& world = world_matrices[i];
        const float4x4& inv_bind = bones_[i].inverse_bind_matrix;
        float4x4& result = out_skinning_matrices[i];

        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                result.m[r][c] = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    result.m[r][c] += world.m[r][k] * inv_bind.m[k][c];
                }
            }
        }
    }
}

} // namespace pictor
