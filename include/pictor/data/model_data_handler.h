#pragma once

#include "pictor/data/model_data_types.h"
#include "pictor/data/vertex_data_uploader.h"
#include "pictor/animation/animation_system.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pictor {

class FBXScene;

/// Configuration for the model data handler
struct ModelDataHandlerConfig {
    uint32_t max_models     = 256;
    uint32_t max_skin_meshes = 1024;
    uint32_t max_rigs       = 256;
};

/// Model data handler — manages 3D model resources (FBX/OBJ/MMD etc.).
/// Handles registration and lifecycle of skin meshes, bones/rigs,
/// and their associated animation data.
///
/// Works in coordination with VertexDataUploader (for GPU mesh data)
/// and AnimationSystem (for skeletons and clips).
class ModelDataHandler {
public:
    ModelDataHandler(VertexDataUploader& vertex_uploader,
                     AnimationSystem& animation_system);
    ~ModelDataHandler();

    ModelDataHandler(const ModelDataHandler&) = delete;
    ModelDataHandler& operator=(const ModelDataHandler&) = delete;

    // ============================================================
    // Model Registration
    // ============================================================

    /// Register a complete model. Internally registers skin meshes,
    /// rig, and animation clips into the appropriate subsystems.
    ModelHandle register_model(const ModelDescriptor& desc);

    /// Unregister a model and all its sub-resources.
    void unregister_model(ModelHandle handle);

    // ============================================================
    // FBX Loading (convenience)
    // ============================================================

    /// Load and register an FBX file. Returns INVALID_MODEL on failure.
    /// Error message is available via last_load_error().
    ModelHandle load_model_from_fbx(const std::string& path);
    /// Load and register an FBX from memory.
    ModelHandle load_model_from_fbx_memory(const uint8_t* data, size_t size,
                                            const std::string& name);

    /// Retrieve the FBXScene associated with a model previously loaded via
    /// load_model_from_fbx(). Returns nullptr if the model was not FBX-sourced.
    std::shared_ptr<FBXScene> get_fbx_scene(ModelHandle handle) const;

    /// Last error message from a load_* call (empty on success).
    const std::string& last_load_error() const noexcept { return last_load_error_; }

    // ============================================================
    // Skin Mesh Operations
    // ============================================================

    /// Register a standalone skin mesh.
    SkinMeshHandle register_skin_mesh(const SkinMeshDescriptor& desc);

    /// Unregister a skin mesh.
    void unregister_skin_mesh(SkinMeshHandle handle);

    /// Get the underlying GPU mesh handle for a skin mesh.
    MeshHandle get_gpu_mesh(SkinMeshHandle handle) const;

    /// Get skin weights for a skin mesh.
    const std::vector<SkinWeight>* get_skin_weights(SkinMeshHandle handle) const;

    /// Get morph target names for a skin mesh.
    const std::vector<std::string>* get_morph_target_names(SkinMeshHandle handle) const;

    // ============================================================
    // Rig Operations
    // ============================================================

    /// Register a rig (bone hierarchy + constraints).
    RigHandle register_rig(const RigDescriptor& desc);

    /// Unregister a rig.
    void unregister_rig(RigHandle handle);

    /// Get the AnimationSystem skeleton handle for a rig.
    SkeletonHandle get_skeleton(RigHandle handle) const;

    /// Get IK chain descriptors for a rig.
    const std::vector<IKChainDescriptor>* get_ik_chains(RigHandle handle) const;

    // ============================================================
    // Query
    // ============================================================

    bool is_valid_model(ModelHandle handle) const;
    bool is_valid_skin_mesh(SkinMeshHandle handle) const;
    bool is_valid_rig(RigHandle handle) const;

    const ModelEntry*    get_model_entry(ModelHandle handle) const;
    const SkinMeshEntry* get_skin_mesh_entry(SkinMeshHandle handle) const;
    const RigEntry*      get_rig_entry(RigHandle handle) const;

    ModelHandle    find_model_by_name(const std::string& name) const;
    SkinMeshHandle find_skin_mesh_by_name(const std::string& name) const;
    RigHandle      find_rig_by_name(const std::string& name) const;

    uint32_t model_count() const     { return static_cast<uint32_t>(models_.size()); }
    uint32_t skin_mesh_count() const { return static_cast<uint32_t>(skin_meshes_.size()); }
    uint32_t rig_count() const       { return static_cast<uint32_t>(rigs_.size()); }

    // ============================================================
    // Stats
    // ============================================================

    struct Stats {
        uint32_t model_count     = 0;
        uint32_t skin_mesh_count = 0;
        uint32_t rig_count       = 0;
        uint32_t total_vertices  = 0;
        uint32_t total_indices   = 0;
        uint32_t total_bones     = 0;
    };

    Stats get_stats() const;

    // ============================================================
    // Format Detection
    // ============================================================

    /// Detect model format from file extension.
    static ModelFormat detect_format(const std::string& path);

    /// Detect model format from file header bytes.
    static ModelFormat detect_format(const uint8_t* data, size_t size);

private:
    VertexDataUploader& vertex_uploader_;
    AnimationSystem&    animation_system_;

    std::vector<ModelEntry>    models_;
    std::vector<SkinMeshEntry> skin_meshes_;
    std::vector<RigEntry>      rigs_;

    std::unordered_map<std::string, ModelHandle>    model_name_map_;
    std::unordered_map<std::string, SkinMeshHandle> skin_mesh_name_map_;
    std::unordered_map<std::string, RigHandle>      rig_name_map_;

    ModelHandle    next_model_handle_     = 0;
    SkinMeshHandle next_skin_mesh_handle_ = 0;
    RigHandle      next_rig_handle_       = 0;

    /// ModelHandle -> owning FBXScene (only populated via load_model_from_fbx).
    std::unordered_map<ModelHandle, std::shared_ptr<FBXScene>> fbx_scenes_;
    std::string last_load_error_;
};

} // namespace pictor
