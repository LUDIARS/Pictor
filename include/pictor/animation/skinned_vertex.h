// Skinned Vertex Layout
//
// Canonical per-vertex layout used by the general-purpose lit shader
// (shaders/lit.vert + shaders/lit.frag). Demos and renderers that want
// skinning can reuse this struct + the packing helpers below.
//
// Layout (must match lit.vert bindings):
//   offset  0: vec3  position      (location 0)
//   offset 12: vec3  normal        (location 1)
//   offset 24: uvec4 joint_indices (location 2)
//   offset 40: vec4  joint_weights (location 3)
//   stride  : 56 bytes
#pragma once

#include "pictor/core/types.h"
#include "pictor/data/model_data_types.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace pictor {

/// CPU-side layout of the canonical skinned vertex.
struct SkinnedVertex {
    float    position[3];
    float    normal[3];
    uint32_t joint_indices[4];
    float    joint_weights[4];
};

static_assert(sizeof(SkinnedVertex) == 56, "SkinnedVertex must be 56 bytes to match lit.vert layout");

/// Vertex attribute offsets (bytes from start of vertex).
constexpr uint32_t SKINNED_VERTEX_OFFSET_POSITION = 0;
constexpr uint32_t SKINNED_VERTEX_OFFSET_NORMAL   = 12;
constexpr uint32_t SKINNED_VERTEX_OFFSET_JOINTS   = 24;
constexpr uint32_t SKINNED_VERTEX_OFFSET_WEIGHTS  = 40;
constexpr uint32_t SKINNED_VERTEX_STRIDE          = 56;

/// Pack triangulated FBX geometry + skin weights into the canonical layout.
/// Positions / normals come from FBXGeometry::Triangulated (per-tri-vertex),
/// joints / weights come from SkinMeshDescriptor (already per-tri-vertex).
inline std::vector<SkinnedVertex> pack_skinned_vertices(
    const float3* positions, size_t position_count,
    const float3* normals,   size_t normal_count,
    const SkinWeight* weights, size_t weight_count)
{
    std::vector<SkinnedVertex> out(position_count);
    for (size_t i = 0; i < position_count; ++i) {
        SkinnedVertex& v = out[i];
        v.position[0] = positions[i].x;
        v.position[1] = positions[i].y;
        v.position[2] = positions[i].z;

        if (i < normal_count) {
            v.normal[0] = normals[i].x;
            v.normal[1] = normals[i].y;
            v.normal[2] = normals[i].z;
        } else {
            v.normal[0] = 0.0f; v.normal[1] = 1.0f; v.normal[2] = 0.0f;
        }

        if (i < weight_count) {
            for (int k = 0; k < 4; ++k) {
                v.joint_indices[k] = weights[i].bone_indices[k];
                v.joint_weights[k] = weights[i].weights[k];
            }
        } else {
            // Unskinned fallback: single weight on bone 0.
            v.joint_indices[0] = 0; v.joint_indices[1] = 0;
            v.joint_indices[2] = 0; v.joint_indices[3] = 0;
            v.joint_weights[0] = 1.0f; v.joint_weights[1] = 0.0f;
            v.joint_weights[2] = 0.0f; v.joint_weights[3] = 0.0f;
        }
    }
    return out;
}

} // namespace pictor
