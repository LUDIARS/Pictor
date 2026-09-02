// Textured, skinned vertex layout shared by every fbx_viewer pipeline
// (model.vert / fur_shell.vert). Kept in one header so the C++ packing
// code and each pipeline's VkVertexInputAttributeDescription agree.
#pragma once

#include <cstdint>

namespace pictor_fbx_viewer {

struct TexturedSkinnedVertex {
    float    position[3];
    float    normal[3];
    float    uv[2];
    uint32_t joint_indices[4];
    float    joint_weights[4];
};
static_assert(sizeof(TexturedSkinnedVertex) == 64,
              "TexturedSkinnedVertex must be 64 bytes to match the vertex shaders");

constexpr uint32_t TSV_OFFSET_POSITION = 0;
constexpr uint32_t TSV_OFFSET_NORMAL   = 12;
constexpr uint32_t TSV_OFFSET_UV       = 24;
constexpr uint32_t TSV_OFFSET_JOINTS   = 32;
constexpr uint32_t TSV_OFFSET_WEIGHTS  = 48;
constexpr uint32_t TSV_STRIDE          = 64;

} // namespace pictor_fbx_viewer
