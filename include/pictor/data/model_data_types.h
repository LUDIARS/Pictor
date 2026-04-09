#pragma once

#include "pictor/core/types.h"
#include "pictor/data/vertex_data_uploader.h"
#include "pictor/animation/animation_types.h"
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace pictor {

// ============================================================
// Model Handle Types
// ============================================================

using ModelHandle     = uint32_t;
using SkinMeshHandle  = uint32_t;
using RigHandle       = uint32_t;

constexpr ModelHandle    INVALID_MODEL     = std::numeric_limits<uint32_t>::max();
constexpr SkinMeshHandle INVALID_SKIN_MESH = std::numeric_limits<uint32_t>::max();
constexpr RigHandle      INVALID_RIG       = std::numeric_limits<uint32_t>::max();

// ============================================================
// Model Source Format
// ============================================================

enum class ModelFormat : uint8_t {
    UNKNOWN  = 0,
    FBX      = 1,
    OBJ      = 2,
    MMD_PMX  = 3,  // MikuMikuDance PMX
    MMD_PMD  = 4,  // MikuMikuDance PMD (legacy)
    GLTF     = 5,
    GLB      = 6,
    COLLADA  = 7   // DAE
};

// ============================================================
// Skin Weight — per-vertex bone influence
// ============================================================

struct SkinWeight {
    uint32_t bone_indices[4] = {};     // Up to 4 bone influences
    float    weights[4]      = {};     // Corresponding weights (sum to 1.0)
};

// ============================================================
// Skin Mesh Descriptor
// ============================================================

struct SkinMeshDescriptor {
    std::string  name;
    VertexLayout layout;

    const void*  vertex_data      = nullptr;
    size_t       vertex_data_size = 0;
    uint32_t     vertex_count     = 0;

    const void*  index_data      = nullptr;
    size_t       index_data_size = 0;
    uint32_t     index_count     = 0;
    bool         index_32bit     = true;

    /// Per-vertex skin weights (vertex_count entries)
    std::vector<SkinWeight> skin_weights;

    /// Morph target / blend shape names
    std::vector<std::string> morph_target_names;

    /// Morph target vertex deltas (morph_count * vertex_count * 3 floats)
    std::vector<float> morph_deltas;
};

// ============================================================
// Rig Descriptor — bone hierarchy + constraints
// ============================================================

struct RigDescriptor {
    std::string        name;
    std::vector<Bone>  bones;

    /// IK chain definitions embedded in the model
    std::vector<IKChainDescriptor> ik_chains;
};

// ============================================================
// Model Descriptor — a complete 3D model resource
// ============================================================

struct ModelDescriptor {
    std::string  name;
    ModelFormat  format = ModelFormat::UNKNOWN;

    /// Skin meshes contained in this model
    std::vector<SkinMeshDescriptor> skin_meshes;

    /// Rig / skeleton
    RigDescriptor rig;

    /// Embedded animation clips
    std::vector<AnimationClipDescriptor> animation_clips;

    /// Material slot names (for external material binding)
    std::vector<std::string> material_slots;

    /// Embedded texture paths (relative to model file)
    std::vector<std::string> texture_paths;
};

// ============================================================
// Skin Mesh Entry — registered skin mesh bookkeeping
// ============================================================

struct SkinMeshEntry {
    SkinMeshHandle handle       = INVALID_SKIN_MESH;
    std::string    name;
    MeshHandle     gpu_mesh     = INVALID_MESH;     // Underlying vertex data
    uint32_t       vertex_count = 0;
    uint32_t       index_count  = 0;

    std::vector<SkinWeight>  skin_weights;
    std::vector<std::string> morph_target_names;
    std::vector<float>       morph_deltas;
};

// ============================================================
// Rig Entry — registered rig bookkeeping
// ============================================================

struct RigEntry {
    RigHandle      handle   = INVALID_RIG;
    std::string    name;
    SkeletonHandle skeleton = INVALID_SKELETON;  // AnimationSystem skeleton
    std::vector<IKChainDescriptor> ik_chains;
};

// ============================================================
// Model Entry — registered model bookkeeping
// ============================================================

struct ModelEntry {
    ModelHandle  handle = INVALID_MODEL;
    std::string  name;
    ModelFormat  format = ModelFormat::UNKNOWN;

    /// Registered skin meshes
    std::vector<SkinMeshHandle> skin_meshes;

    /// Registered rig
    RigHandle rig = INVALID_RIG;

    /// Registered animation clips
    std::vector<AnimationClipHandle> animation_clips;

    /// Material slot names
    std::vector<std::string> material_slots;

    /// Texture paths
    std::vector<std::string> texture_paths;
};

} // namespace pictor
