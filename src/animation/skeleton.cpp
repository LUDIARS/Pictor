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
            // Child bone — cascade through parent's world matrix.
            //
            // Pictor uses row-vector / row-major matrices (translation at
            // m[3][*], `v_row * M`). A point in the child's local frame
            // reaches world via:
            //   v_world = v_child_local * local_matrix * parent_world
            // which means the stored result is `local * parent` — NOT
            // `parent * local`. The earlier `parent * local` form is the
            // column-vector expression and was incorrect for Pictor's
            // storage convention (see fbx_scene.cpp for the matching fix).
            const float4x4& parent = out_world_matrices[bones_[i].parent_index];
            float4x4& result = out_world_matrices[i];
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    result.m[r][c] = 0.0f;
                    for (int k = 0; k < 4; ++k) {
                        result.m[r][c] += local_matrix.m[r][k] * parent.m[k][c];
                    }
                }
            }
        }
    }
}

void Skeleton::compute_skinning_matrices(const Transform* local_transforms,
                                         float4x4* out_skinning_matrices) const {
    // First compute world matrices.
    std::vector<float4x4> world_matrices(bones_.size());
    compute_world_matrices(local_transforms, world_matrices.data());

    // Then produce the per-bone skinning transform.
    //
    // For a vertex V in mesh world space at bind time, the animated world
    // position is:
    //   V_animated = V_bind * inverse_bind * current_world  (row-vector)
    // i.e. "bring into bone-local at bind", then "re-position by the
    // current world". The stored skinning matrix is therefore
    //   S = inverse_bind * current_world                    (row-vector order)
    // which is `mat_mul(inverse_bind, current_world)`.
    //
    // When the caller transposes this for GLSL (column-vector `S * v`),
    // the uploaded matrix is current_world_col * inverse_bind_col, which
    // is the canonical column-vector skinning formula.
    for (uint32_t i = 0; i < bones_.size(); ++i) {
        const float4x4& world    = world_matrices[i];
        const float4x4& inv_bind = bones_[i].inverse_bind_matrix;
        float4x4&       result   = out_skinning_matrices[i];

        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                result.m[r][c] = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    result.m[r][c] += inv_bind.m[r][k] * world.m[k][c];
                }
            }
        }
    }
}

} // namespace pictor
