#pragma once

#include "pictor/data/texture_registry.h"
#include "pictor/data/vertex_data_uploader.h"
#include <memory>

namespace pictor {

/// Central data handler — facade for texture and vertex data management.
/// Provides a unified interface for registering, uploading, and querying
/// GPU resources. Designed for use by both the renderer internals and
/// external tools/editors.
class DataHandler {
public:
    DataHandler(GpuMemoryAllocator& gpu_allocator, GPUBufferManager& buffer_manager);
    ~DataHandler();

    DataHandler(const DataHandler&) = delete;
    DataHandler& operator=(const DataHandler&) = delete;

    // ---- Texture Operations ----

    TextureHandle register_texture(const TextureDescriptor& desc);
    bool upload_texture_data(TextureHandle handle, const void* data, size_t size,
                             uint32_t mip_level = 0, uint32_t array_layer = 0);
    void unregister_texture(TextureHandle handle);

    // ---- Mesh/Vertex Operations ----

    MeshHandle register_mesh(const MeshDataDescriptor& desc);
    bool upload_vertex_data(MeshHandle handle, const void* data, size_t size);
    bool upload_index_data(MeshHandle handle, const void* data, size_t size);
    bool update_vertex_region(MeshHandle handle, size_t offset, const void* data, size_t size);
    void unregister_mesh(MeshHandle handle);

    // ---- Subsystem Access ----

    TextureRegistry&      textures()       { return *texture_registry_; }
    const TextureRegistry& textures() const { return *texture_registry_; }

    VertexDataUploader&       meshes()        { return *vertex_uploader_; }
    const VertexDataUploader& meshes() const  { return *vertex_uploader_; }

    // ---- Aggregate Stats ----

    struct Stats {
        TextureRegistry::Stats    texture_stats;
        VertexDataUploader::Stats mesh_stats;
    };

    Stats get_stats() const;

private:
    std::unique_ptr<TextureRegistry>    texture_registry_;
    std::unique_ptr<VertexDataUploader> vertex_uploader_;
};

} // namespace pictor
