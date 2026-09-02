// Finds the character's eyes from its albedo texture so effects (tears)
// can be anchored without hand-authored sockets.
//
// This viewer's plush test textures paint the eyes as the only large dark
// blobs; we take the two biggest dark connected components in the image,
// then pick the mesh vertex whose UV is nearest each blob centre. The
// vertex index keeps the anchor skinned (its joints/weights are reused).
#pragma once

#include "textured_skinned_vertex.h"

#include <cstdint>
#include <vector>

namespace pictor_fbx_viewer {

struct EyeAnchor {
    uint32_t vertex_index = UINT32_MAX;   // into the packed mesh
    float    uv[2]        = {0.0f, 0.0f};
    float    blob_area_px = 0.0f;
    float    uv_distance_sq = 0.0f;        // blob centre to selected mesh UV
};

/// `rgba` is w*h*4 top-down (stb layout, same orientation the shaders
/// sample with). Returns 0..2 anchors, left/right order not guaranteed.
std::vector<EyeAnchor> locate_eyes(const uint8_t* rgba, int w, int h,
                                   const std::vector<TexturedSkinnedVertex>& vertices,
                                   const std::vector<uint32_t>& indices,
                                   uint32_t index_start,
                                   uint32_t index_count);

} // namespace pictor_fbx_viewer
