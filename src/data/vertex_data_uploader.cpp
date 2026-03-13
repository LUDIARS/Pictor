#include "pictor/data/vertex_data_uploader.h"

namespace pictor {

VertexDataUploader::VertexDataUploader(GPUBufferManager& buffer_manager)
    : buffer_manager_(buffer_manager)
{
}

VertexDataUploader::~VertexDataUploader() {
    // Free all GPU allocations
    for (auto& entry : entries_) {
        if (entry.gpu_allocation.vertex_buffer.valid || entry.gpu_allocation.index_buffer.valid) {
            buffer_manager_.free_mesh(entry.gpu_allocation);
        }
    }
}

MeshHandle VertexDataUploader::register_mesh(const MeshDataDescriptor& desc) {
    MeshHandle handle = next_handle_++;

    MeshDataEntry entry;
    entry.handle       = handle;
    entry.name         = desc.name;
    entry.layout       = desc.layout;
    entry.vertex_count = desc.vertex_count;
    entry.index_count  = desc.index_count;
    entry.index_32bit  = desc.index_32bit;

    // Allocate GPU buffers
    entry.gpu_allocation = buffer_manager_.allocate_mesh(
        desc.vertex_data_size > 0 ? desc.vertex_data_size : 0,
        desc.index_data_size > 0 ? desc.index_data_size : 0
    );
    entry.gpu_allocation.vertex_count = desc.vertex_count;
    entry.gpu_allocation.index_count = desc.index_count;

    // Upload initial data if provided
    if (desc.vertex_data && desc.vertex_data_size > 0) {
        // Stage vertex data for GPU upload
        GpuAllocation staging = buffer_manager_.allocate_staging(desc.vertex_data_size);
        if (staging.valid) {
            // In production: memcpy to staging, record copy command
            entry.uploaded = true;
        }
    }

    if (desc.index_data && desc.index_data_size > 0) {
        GpuAllocation staging = buffer_manager_.allocate_staging(desc.index_data_size);
        if (staging.valid) {
            // In production: memcpy to staging, record copy command
            entry.uploaded = entry.uploaded; // Keep previous state
        }
    }

    // Register in name map
    if (!desc.name.empty()) {
        name_map_[desc.name] = handle;
    }

    entries_.push_back(std::move(entry));
    return handle;
}

bool VertexDataUploader::upload_vertex_data(MeshHandle handle, const void* data, size_t size) {
    if (!data || size == 0) return false;

    for (auto& entry : entries_) {
        if (entry.handle == handle) {
            GpuAllocation staging = buffer_manager_.allocate_staging(size);
            if (!staging.valid) return false;

            // Stage and transfer
            entry.uploaded = true;
            return true;
        }
    }
    return false;
}

bool VertexDataUploader::upload_index_data(MeshHandle handle, const void* data, size_t size) {
    if (!data || size == 0) return false;

    for (auto& entry : entries_) {
        if (entry.handle == handle) {
            GpuAllocation staging = buffer_manager_.allocate_staging(size);
            if (!staging.valid) return false;

            entry.uploaded = true;
            return true;
        }
    }
    return false;
}

bool VertexDataUploader::update_vertex_region(MeshHandle handle, size_t offset,
                                               const void* data, size_t size) {
    if (!data || size == 0) return false;

    for (auto& entry : entries_) {
        if (entry.handle == handle) {
            if (!entry.gpu_allocation.vertex_buffer.valid) return false;
            if (offset + size > entry.gpu_allocation.vertex_buffer.size) return false;

            GpuAllocation staging = buffer_manager_.allocate_staging(size);
            if (!staging.valid) return false;

            // In production: memcpy to staging at offset, record partial copy
            return true;
        }
    }
    return false;
}

void VertexDataUploader::unregister_mesh(MeshHandle handle) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->handle == handle) {
            buffer_manager_.free_mesh(it->gpu_allocation);
            if (!it->name.empty()) {
                name_map_.erase(it->name);
            }
            if (it != entries_.end() - 1) {
                *it = std::move(entries_.back());
            }
            entries_.pop_back();
            return;
        }
    }
}

bool VertexDataUploader::is_valid(MeshHandle handle) const {
    for (const auto& entry : entries_) {
        if (entry.handle == handle) return true;
    }
    return false;
}

const MeshDataEntry* VertexDataUploader::get_entry(MeshHandle handle) const {
    for (const auto& entry : entries_) {
        if (entry.handle == handle) return &entry;
    }
    return nullptr;
}

MeshHandle VertexDataUploader::find_by_name(const std::string& name) const {
    auto it = name_map_.find(name);
    return (it != name_map_.end()) ? it->second : INVALID_MESH;
}

std::vector<MeshHandle> VertexDataUploader::all_handles() const {
    std::vector<MeshHandle> handles;
    handles.reserve(entries_.size());
    for (const auto& entry : entries_) {
        handles.push_back(entry.handle);
    }
    return handles;
}

VertexDataUploader::Stats VertexDataUploader::get_stats() const {
    Stats stats;
    stats.mesh_count = static_cast<uint32_t>(entries_.size());
    for (const auto& entry : entries_) {
        stats.total_vertex_bytes += entry.gpu_allocation.vertex_buffer.size;
        stats.total_index_bytes += entry.gpu_allocation.index_buffer.size;
        if (entry.uploaded) ++stats.uploaded_count;
        else ++stats.pending_count;
    }
    return stats;
}

} // namespace pictor
