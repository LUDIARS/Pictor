#pragma once
#include "textured_skinned_vertex.h"
#include "pictor/animation/fbx_scene.h"
#include <string>
#include <vector>

namespace pictor_fbx_viewer {
struct SubMesh {
    uint32_t index_start = 0;
    uint32_t index_count = 0;
    pictor::FBXObjectId material_id = 0;
    std::string texture_basename;
    std::string debug_name;
};
struct TopologyVertex {
    uint64_t geometry = 0;
    uint32_t vertex = 0;
    auto operator<=>(const TopologyVertex&) const = default;
};
struct PackedMesh {
    std::vector<TexturedSkinnedVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<SubMesh> submeshes;
    pictor::float3 center{};
    float radius = 1.0f;
    // Import-only connectivity: UV/material corner splits must not split geometry.
    // Not serialized by the legacy viewer cache; reconstruction bypasses that cache.
    std::vector<TopologyVertex> topology;
};
}
