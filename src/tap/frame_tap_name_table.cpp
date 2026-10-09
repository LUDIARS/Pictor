#include "pictor/tap/frame_tap_name_table.h"

#include <limits>
#include <utility>

namespace pictor {

namespace {

/// ハンドルを添字にした表を必要な長さまで伸ばす。 無効ハンドル (uint32 最大) は
/// 表に入れない — 4G 要素の確保を招くため。
template <typename T>
T* slot_for(std::vector<T>& table, uint32_t handle) {
    if (handle == std::numeric_limits<uint32_t>::max()) return nullptr;
    if (handle >= table.size()) table.resize(static_cast<size_t>(handle) + 1);
    return &table[handle];
}

template <typename T>
const T* find_slot(const std::vector<T>& table, uint32_t handle) {
    return handle < table.size() ? &table[handle] : nullptr;
}

} // namespace

void FrameTapNameTable::set_mesh(MeshHandle mesh, std::string name,
                                 uint32_t vertex_count, uint32_t index_count) {
    MeshEntry* entry = slot_for(meshes_, mesh);
    if (!entry) return;
    entry->name         = std::move(name);
    entry->vertex_count = vertex_count;
    entry->index_count  = index_count;
}

void FrameTapNameTable::set_mesh_name(MeshHandle mesh, std::string name) {
    MeshEntry* entry = slot_for(meshes_, mesh);
    if (entry) entry->name = std::move(name);
}

void FrameTapNameTable::clear_mesh(MeshHandle mesh) {
    if (mesh < meshes_.size()) meshes_[mesh] = MeshEntry{};
}

void FrameTapNameTable::set_material_name(MaterialHandle material, std::string name) {
    std::string* entry = slot_for(materials_, material);
    if (entry) *entry = std::move(name);
}

void FrameTapNameTable::clear_material(MaterialHandle material) {
    if (material < materials_.size()) materials_[material].clear();
}

void FrameTapNameTable::set_ui_texture_name(uint32_t texture_id, std::string name) {
    std::string* entry = slot_for(ui_textures_, texture_id);
    if (entry) *entry = std::move(name);
}

void FrameTapNameTable::clear_ui_texture(uint32_t texture_id) {
    if (texture_id < ui_textures_.size()) ui_textures_[texture_id].clear();
}

FrameTapMeshInfo FrameTapNameTable::mesh_info(MeshHandle mesh) const {
    const MeshEntry* entry = find_slot(meshes_, mesh);
    if (!entry) return {};
    return {entry->name, entry->vertex_count, entry->index_count};
}

std::string_view FrameTapNameTable::material_name(MaterialHandle material) const {
    const std::string* entry = find_slot(materials_, material);
    return entry ? std::string_view(*entry) : std::string_view();
}

std::string_view FrameTapNameTable::ui_texture_name(uint32_t texture_id) const {
    const std::string* entry = find_slot(ui_textures_, texture_id);
    return entry ? std::string_view(*entry) : std::string_view();
}

} // namespace pictor
