#pragma once

#include "pictor/core/types.h"
#include "pictor/gpu/gpu_buffer_manager.h"
#include <vector>
#include <string>
#include <unordered_map>

namespace pictor {

/// Describes a vertex layout (set of attributes + stride)
struct VertexLayout {
    std::vector<VertexAttribute> attributes;
    uint32_t stride = 0;  // Bytes per vertex. 0 = auto-compute from attributes.

    /// Compute stride from attributes if not set
    uint32_t computed_stride() const {
        if (stride > 0) return stride;
        uint32_t max_end = 0;
        for (const auto& attr : attributes) {
            uint32_t end = attr.offset + static_cast<uint32_t>(vertex_attribute_size(attr.type));
            if (end > max_end) max_end = end;
        }
        return max_end;
    }
};

/// Descriptor for registering a mesh with flexible vertex data
struct MeshDataDescriptor {
    std::string    name;                     // Debug/query name
    VertexLayout   layout;                   // Vertex format description
    const void*    vertex_data  = nullptr;   // Raw vertex data
    size_t         vertex_data_size = 0;     // Size in bytes
    uint32_t       vertex_count = 0;
    const void*    index_data   = nullptr;   // Optional index data
    size_t         index_data_size = 0;      // Size in bytes
    uint32_t       index_count  = 0;
    bool           index_32bit  = true;      // true=uint32, false=uint16
};

/// Bookkeeping entry for a registered mesh
struct MeshDataEntry {
    MeshHandle    handle          = INVALID_MESH;
    std::string   name;
    VertexLayout  layout;
    uint32_t      vertex_count    = 0;
    uint32_t      index_count     = 0;
    bool          index_32bit     = true;
    GPUBufferManager::MeshAllocation gpu_allocation;
    bool          uploaded        = false;
};

/// Flexible vertex data registration and GPU transfer.
/// Supports arbitrary vertex formats, deferred uploads, and partial updates.
class VertexDataUploader {
public:
    explicit VertexDataUploader(GPUBufferManager& buffer_manager);
    ~VertexDataUploader();

    VertexDataUploader(const VertexDataUploader&) = delete;
    VertexDataUploader& operator=(const VertexDataUploader&) = delete;

    // ---- Registration ----

    /// Register mesh data with a flexible vertex layout.
    /// If vertex_data/index_data are provided, stages for GPU upload.
    MeshHandle register_mesh(const MeshDataDescriptor& desc);

    /// Upload/replace vertex data for an existing mesh.
    bool upload_vertex_data(MeshHandle handle, const void* data, size_t size);

    /// Upload/replace index data for an existing mesh.
    bool upload_index_data(MeshHandle handle, const void* data, size_t size);

    /// Update a sub-region of vertex data (partial update).
    bool update_vertex_region(MeshHandle handle, size_t offset, const void* data, size_t size);

    /// Unregister a mesh and free its GPU memory.
    void unregister_mesh(MeshHandle handle);

    // ---- Query ----

    bool is_valid(MeshHandle handle) const;
    const MeshDataEntry* get_entry(MeshHandle handle) const;
    MeshHandle find_by_name(const std::string& name) const;
    std::vector<MeshHandle> all_handles() const;
    uint32_t count() const { return static_cast<uint32_t>(entries_.size()); }

    // ---- Stats ----

    struct Stats {
        uint32_t mesh_count      = 0;
        size_t   total_vertex_bytes = 0;
        size_t   total_index_bytes  = 0;
        uint32_t uploaded_count  = 0;
        uint32_t pending_count   = 0;
    };

    Stats get_stats() const;

private:
    GPUBufferManager& buffer_manager_;
    std::vector<MeshDataEntry> entries_;
    std::unordered_map<std::string, MeshHandle> name_map_;
    MeshHandle next_handle_ = 0;
};

} // namespace pictor
