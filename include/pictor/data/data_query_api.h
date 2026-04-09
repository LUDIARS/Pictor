#pragma once

#include "pictor/data/data_handler.h"
#include <string>
#include <vector>
#include <functional>

namespace pictor {

/// Read-only snapshot of a texture for external introspection
struct TextureInfo {
    TextureHandle handle     = INVALID_TEXTURE;
    std::string   name;
    uint32_t      width      = 0;
    uint32_t      height     = 0;
    uint32_t      depth      = 1;
    uint32_t      mip_levels = 1;
    uint32_t      array_layers = 1;
    TextureFormat format     = TextureFormat::RGBA8_UNORM;
    TextureType   type       = TextureType::TEXTURE_2D;
    size_t        gpu_bytes  = 0;
    bool          uploaded   = false;
};

/// Read-only snapshot of a mesh for external introspection
struct MeshInfo {
    MeshHandle    handle        = INVALID_MESH;
    std::string   name;
    VertexLayout  layout;
    uint32_t      vertex_count  = 0;
    uint32_t      index_count   = 0;
    bool          index_32bit   = true;
    size_t        vertex_bytes  = 0;
    size_t        index_bytes   = 0;
    bool          uploaded      = false;
};

/// Summary of all registered data
struct DataSummary {
    uint32_t total_textures    = 0;
    uint32_t total_meshes      = 0;
    size_t   total_texture_gpu_bytes = 0;
    size_t   total_mesh_gpu_bytes    = 0;
    uint32_t textures_uploaded = 0;
    uint32_t meshes_uploaded   = 0;
};

/// Read-only query API for external editors and tools.
///
/// This API provides a safe, non-mutating view into all registered data.
/// External tools (level editors, asset browsers, debug viewers) can use
/// this to enumerate and inspect textures, meshes, and their GPU state
/// without affecting the rendering pipeline.
///
/// Usage example:
///   DataQueryAPI query(renderer.data_handler());
///   auto summary = query.get_summary();
///   query.for_each_texture([](const TextureInfo& info) {
///       printf("Texture: %s (%dx%d)\n", info.name.c_str(), info.width, info.height);
///   });
class DataQueryAPI {
public:
    explicit DataQueryAPI(const DataHandler& handler);
    ~DataQueryAPI();

    // ---- Summary ----

    /// Get an aggregate summary of all registered data
    DataSummary get_summary() const;

    // ---- Texture Queries ----

    /// Get info for a specific texture
    TextureInfo get_texture_info(TextureHandle handle) const;

    /// Get info for all registered textures
    std::vector<TextureInfo> get_all_textures() const;

    /// Find texture info by name
    TextureInfo find_texture(const std::string& name) const;

    /// Iterate all textures with a callback
    void for_each_texture(std::function<void(const TextureInfo&)> callback) const;

    // ---- Mesh Queries ----

    /// Get info for a specific mesh
    MeshInfo get_mesh_info(MeshHandle handle) const;

    /// Get info for all registered meshes
    std::vector<MeshInfo> get_all_meshes() const;

    /// Find mesh info by name
    MeshInfo find_mesh(const std::string& name) const;

    /// Iterate all meshes with a callback
    void for_each_mesh(std::function<void(const MeshInfo&)> callback) const;

    // ---- Filtering ----

    /// Get textures matching a format filter
    std::vector<TextureInfo> get_textures_by_format(TextureFormat format) const;

    /// Get meshes that contain a specific vertex semantic
    std::vector<MeshInfo> get_meshes_by_semantic(VertexSemantic semantic) const;

    // ---- JSON Export (for external tools/editors) ----

    /// Export all registered data as a JSON string
    std::string export_json() const;

    /// Export texture list as a JSON string
    std::string export_textures_json() const;

    /// Export mesh list as a JSON string
    std::string export_meshes_json() const;

private:
    const DataHandler& handler_;
};

} // namespace pictor
