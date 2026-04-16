// FBX Importer -- Level 4 (Facade)
//
// Uses FBXDocument (Level 1) + FBXScene (Level 2+3) to project the full
// FBX content into Pictor runtime descriptors (Skeleton, AnimationClip,
// SkinMesh, material slots, texture paths).
#pragma once

#include "pictor/animation/animation_clip.h"
#include "pictor/animation/animation_types.h"
#include "pictor/animation/fbx_scene.h"
#include "pictor/animation/skeleton.h"
#include "pictor/data/model_data_types.h"

#include <memory>
#include <string>
#include <vector>

namespace pictor {

/// Result of an FBX import.
/// Backward-compatible with the previous skeleton-only fields.
struct FBXImportResult {
    bool             success         = false;
    std::string      error_message;
    AnimationFormat  detected_format = AnimationFormat::UNKNOWN;

    /// Full typed scene (owns all parsed data).
    std::shared_ptr<FBXScene>            scene;

    // Projected Pictor runtime descriptors.
    SkeletonDescriptor                   skeleton;
    std::vector<AnimationClipDescriptor> clips;
    std::vector<SkinMeshDescriptor>      skin_meshes;
    std::vector<std::string>             material_slots;
    std::vector<std::string>             texture_paths;
    std::vector<IKChainDescriptor>       ik_chains;

    /// Materialize a ModelDescriptor suitable for
    /// ModelDataHandler::register_model().
    ModelDescriptor to_model_descriptor(const std::string& name) const;

    // Resource ID access (forwards to scene).
    const FBXObject*         get_resource(FBXObjectId id) const noexcept;
    std::vector<FBXObjectId> all_resource_ids() const;
    std::vector<FBXObjectId> resource_ids_of_type(FBXObjectType type) const;
};

class FBXImporter {
public:
    FBXImporter() = default;
    ~FBXImporter() = default;

    /// Import from file path.
    FBXImportResult import_file(const std::string& path) const;
    /// Import from memory buffer.
    FBXImportResult import_memory(const uint8_t* data, size_t size) const;
    /// Detect whether the data is binary or ASCII FBX.
    static AnimationFormat detect_format(const uint8_t* data, size_t size);

private:
    /// Core projection: consume an FBXScene and populate `out`.
    void project(const FBXScene& scene, FBXImportResult& out) const;
    /// Build skeleton from LimbNode Models + (optional) Clusters for bind matrices.
    void build_skeleton(const FBXScene& scene, FBXImportResult& out) const;
    /// Build animation clips from AnimationStack / Layer / CurveNode / Curve.
    void build_clips(const FBXScene& scene, FBXImportResult& out) const;
    /// Build skinned meshes from Geometry + Skin + Cluster.
    void build_skin_meshes(const FBXScene& scene, FBXImportResult& out) const;
    /// Collect material names and texture paths.
    void collect_materials(const FBXScene& scene, FBXImportResult& out) const;
};

} // namespace pictor
