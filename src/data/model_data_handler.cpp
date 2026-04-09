#include "pictor/data/model_data_handler.h"
#include <algorithm>
#include <cstring>

namespace pictor {

ModelDataHandler::ModelDataHandler(VertexDataUploader& vertex_uploader,
                                   AnimationSystem& animation_system)
    : vertex_uploader_(vertex_uploader)
    , animation_system_(animation_system)
{
}

ModelDataHandler::~ModelDataHandler() = default;

// ============================================================
// Model Registration
// ============================================================

ModelHandle ModelDataHandler::register_model(const ModelDescriptor& desc) {
    ModelEntry entry;
    entry.handle = next_model_handle_++;
    entry.name   = desc.name;
    entry.format = desc.format;
    entry.material_slots = desc.material_slots;
    entry.texture_paths  = desc.texture_paths;

    // Register skin meshes
    for (const auto& sm_desc : desc.skin_meshes) {
        SkinMeshHandle sm = register_skin_mesh(sm_desc);
        entry.skin_meshes.push_back(sm);
    }

    // Register rig
    if (!desc.rig.bones.empty()) {
        entry.rig = register_rig(desc.rig);
    }

    // Register animation clips
    for (const auto& clip_desc : desc.animation_clips) {
        AnimationClipHandle clip = animation_system_.register_clip(clip_desc);
        entry.animation_clips.push_back(clip);
    }

    if (!desc.name.empty()) {
        model_name_map_[desc.name] = entry.handle;
    }

    models_.push_back(std::move(entry));
    return models_.back().handle;
}

void ModelDataHandler::unregister_model(ModelHandle handle) {
    auto it = std::find_if(models_.begin(), models_.end(),
        [handle](const ModelEntry& e) { return e.handle == handle; });
    if (it == models_.end()) return;

    // Unregister sub-resources
    for (auto sm : it->skin_meshes) {
        unregister_skin_mesh(sm);
    }
    if (it->rig != INVALID_RIG) {
        unregister_rig(it->rig);
    }
    for (auto clip : it->animation_clips) {
        animation_system_.unregister_clip(clip);
    }

    if (!it->name.empty()) {
        model_name_map_.erase(it->name);
    }

    models_.erase(it);
}

// ============================================================
// Skin Mesh Operations
// ============================================================

SkinMeshHandle ModelDataHandler::register_skin_mesh(const SkinMeshDescriptor& desc) {
    SkinMeshEntry entry;
    entry.handle       = next_skin_mesh_handle_++;
    entry.name         = desc.name;
    entry.vertex_count = desc.vertex_count;
    entry.index_count  = desc.index_count;
    entry.skin_weights = desc.skin_weights;
    entry.morph_target_names = desc.morph_target_names;
    entry.morph_deltas = desc.morph_deltas;

    // Register the underlying vertex/index data with VertexDataUploader
    MeshDataDescriptor mesh_desc;
    mesh_desc.name            = desc.name;
    mesh_desc.layout          = desc.layout;
    mesh_desc.vertex_data     = desc.vertex_data;
    mesh_desc.vertex_data_size = desc.vertex_data_size;
    mesh_desc.vertex_count    = desc.vertex_count;
    mesh_desc.index_data      = desc.index_data;
    mesh_desc.index_data_size = desc.index_data_size;
    mesh_desc.index_count     = desc.index_count;
    mesh_desc.index_32bit     = desc.index_32bit;

    entry.gpu_mesh = vertex_uploader_.register_mesh(mesh_desc);

    if (!desc.name.empty()) {
        skin_mesh_name_map_[desc.name] = entry.handle;
    }

    skin_meshes_.push_back(std::move(entry));
    return skin_meshes_.back().handle;
}

void ModelDataHandler::unregister_skin_mesh(SkinMeshHandle handle) {
    auto it = std::find_if(skin_meshes_.begin(), skin_meshes_.end(),
        [handle](const SkinMeshEntry& e) { return e.handle == handle; });
    if (it == skin_meshes_.end()) return;

    if (it->gpu_mesh != INVALID_MESH) {
        vertex_uploader_.unregister_mesh(it->gpu_mesh);
    }

    if (!it->name.empty()) {
        skin_mesh_name_map_.erase(it->name);
    }

    skin_meshes_.erase(it);
}

MeshHandle ModelDataHandler::get_gpu_mesh(SkinMeshHandle handle) const {
    auto it = std::find_if(skin_meshes_.begin(), skin_meshes_.end(),
        [handle](const SkinMeshEntry& e) { return e.handle == handle; });
    return (it != skin_meshes_.end()) ? it->gpu_mesh : INVALID_MESH;
}

const std::vector<SkinWeight>* ModelDataHandler::get_skin_weights(SkinMeshHandle handle) const {
    auto it = std::find_if(skin_meshes_.begin(), skin_meshes_.end(),
        [handle](const SkinMeshEntry& e) { return e.handle == handle; });
    return (it != skin_meshes_.end()) ? &it->skin_weights : nullptr;
}

const std::vector<std::string>* ModelDataHandler::get_morph_target_names(SkinMeshHandle handle) const {
    auto it = std::find_if(skin_meshes_.begin(), skin_meshes_.end(),
        [handle](const SkinMeshEntry& e) { return e.handle == handle; });
    return (it != skin_meshes_.end()) ? &it->morph_target_names : nullptr;
}

// ============================================================
// Rig Operations
// ============================================================

RigHandle ModelDataHandler::register_rig(const RigDescriptor& desc) {
    RigEntry entry;
    entry.handle    = next_rig_handle_++;
    entry.name      = desc.name;
    entry.ik_chains = desc.ik_chains;

    // Register skeleton in AnimationSystem
    SkeletonDescriptor skel_desc;
    skel_desc.name  = desc.name;
    skel_desc.bones = desc.bones;
    entry.skeleton = animation_system_.register_skeleton(skel_desc);

    if (!desc.name.empty()) {
        rig_name_map_[desc.name] = entry.handle;
    }

    rigs_.push_back(std::move(entry));
    return rigs_.back().handle;
}

void ModelDataHandler::unregister_rig(RigHandle handle) {
    auto it = std::find_if(rigs_.begin(), rigs_.end(),
        [handle](const RigEntry& e) { return e.handle == handle; });
    if (it == rigs_.end()) return;

    if (it->skeleton != INVALID_SKELETON) {
        animation_system_.unregister_skeleton(it->skeleton);
    }

    if (!it->name.empty()) {
        rig_name_map_.erase(it->name);
    }

    rigs_.erase(it);
}

SkeletonHandle ModelDataHandler::get_skeleton(RigHandle handle) const {
    auto it = std::find_if(rigs_.begin(), rigs_.end(),
        [handle](const RigEntry& e) { return e.handle == handle; });
    return (it != rigs_.end()) ? it->skeleton : INVALID_SKELETON;
}

const std::vector<IKChainDescriptor>* ModelDataHandler::get_ik_chains(RigHandle handle) const {
    auto it = std::find_if(rigs_.begin(), rigs_.end(),
        [handle](const RigEntry& e) { return e.handle == handle; });
    return (it != rigs_.end()) ? &it->ik_chains : nullptr;
}

// ============================================================
// Query
// ============================================================

bool ModelDataHandler::is_valid_model(ModelHandle handle) const {
    return std::any_of(models_.begin(), models_.end(),
        [handle](const ModelEntry& e) { return e.handle == handle; });
}

bool ModelDataHandler::is_valid_skin_mesh(SkinMeshHandle handle) const {
    return std::any_of(skin_meshes_.begin(), skin_meshes_.end(),
        [handle](const SkinMeshEntry& e) { return e.handle == handle; });
}

bool ModelDataHandler::is_valid_rig(RigHandle handle) const {
    return std::any_of(rigs_.begin(), rigs_.end(),
        [handle](const RigEntry& e) { return e.handle == handle; });
}

const ModelEntry* ModelDataHandler::get_model_entry(ModelHandle handle) const {
    auto it = std::find_if(models_.begin(), models_.end(),
        [handle](const ModelEntry& e) { return e.handle == handle; });
    return (it != models_.end()) ? &(*it) : nullptr;
}

const SkinMeshEntry* ModelDataHandler::get_skin_mesh_entry(SkinMeshHandle handle) const {
    auto it = std::find_if(skin_meshes_.begin(), skin_meshes_.end(),
        [handle](const SkinMeshEntry& e) { return e.handle == handle; });
    return (it != skin_meshes_.end()) ? &(*it) : nullptr;
}

const RigEntry* ModelDataHandler::get_rig_entry(RigHandle handle) const {
    auto it = std::find_if(rigs_.begin(), rigs_.end(),
        [handle](const RigEntry& e) { return e.handle == handle; });
    return (it != rigs_.end()) ? &(*it) : nullptr;
}

ModelHandle ModelDataHandler::find_model_by_name(const std::string& name) const {
    auto it = model_name_map_.find(name);
    return (it != model_name_map_.end()) ? it->second : INVALID_MODEL;
}

SkinMeshHandle ModelDataHandler::find_skin_mesh_by_name(const std::string& name) const {
    auto it = skin_mesh_name_map_.find(name);
    return (it != skin_mesh_name_map_.end()) ? it->second : INVALID_SKIN_MESH;
}

RigHandle ModelDataHandler::find_rig_by_name(const std::string& name) const {
    auto it = rig_name_map_.find(name);
    return (it != rig_name_map_.end()) ? it->second : INVALID_RIG;
}

// ============================================================
// Stats
// ============================================================

ModelDataHandler::Stats ModelDataHandler::get_stats() const {
    Stats stats;
    stats.model_count     = static_cast<uint32_t>(models_.size());
    stats.skin_mesh_count = static_cast<uint32_t>(skin_meshes_.size());
    stats.rig_count       = static_cast<uint32_t>(rigs_.size());

    for (const auto& sm : skin_meshes_) {
        stats.total_vertices += sm.vertex_count;
        stats.total_indices  += sm.index_count;
    }

    for (const auto& rig : rigs_) {
        if (rig.skeleton != INVALID_SKELETON) {
            const Skeleton* skel = animation_system_.get_skeleton(rig.skeleton);
            if (skel) {
                stats.total_bones += skel->bone_count();
            }
        }
    }

    return stats;
}

// ============================================================
// Format Detection
// ============================================================

ModelFormat ModelDataHandler::detect_format(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return ModelFormat::UNKNOWN;

    std::string ext = path.substr(dot + 1);
    // Convert to lowercase
    for (auto& c : ext) {
        if (c >= 'A' && c <= 'Z') c += 32;
    }

    if (ext == "fbx")                 return ModelFormat::FBX;
    if (ext == "obj")                 return ModelFormat::OBJ;
    if (ext == "pmx")                 return ModelFormat::MMD_PMX;
    if (ext == "pmd")                 return ModelFormat::MMD_PMD;
    if (ext == "gltf")                return ModelFormat::GLTF;
    if (ext == "glb")                 return ModelFormat::GLB;
    if (ext == "dae")                 return ModelFormat::COLLADA;
    return ModelFormat::UNKNOWN;
}

ModelFormat ModelDataHandler::detect_format(const uint8_t* data, size_t size) {
    if (!data || size < 4) return ModelFormat::UNKNOWN;

    // FBX binary: starts with "Kaydara FBX Binary"
    if (size >= 20 && std::memcmp(data, "Kaydara FBX Binary", 18) == 0) {
        return ModelFormat::FBX;
    }

    // glTF binary (GLB): magic 0x46546C67
    if (size >= 4 && data[0] == 0x67 && data[1] == 0x6C &&
        data[2] == 0x54 && data[3] == 0x46) {
        return ModelFormat::GLB;
    }

    // PMX: magic "PMX "
    if (size >= 4 && data[0] == 'P' && data[1] == 'M' &&
        data[2] == 'X' && data[3] == ' ') {
        return ModelFormat::MMD_PMX;
    }

    // PMD: magic "Pmd"
    if (size >= 3 && data[0] == 'P' && data[1] == 'm' && data[2] == 'd') {
        return ModelFormat::MMD_PMD;
    }

    return ModelFormat::UNKNOWN;
}

} // namespace pictor
