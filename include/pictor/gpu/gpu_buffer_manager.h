#pragma once

#include "pictor/core/types.h"
#include "pictor/memory/gpu_memory_allocator.h"
#include "pictor/scene/scene_registry.h"
#include <vector>

namespace pictor {

/// GPU buffer manager for SoA SSBOs, mesh pool, and indirect draw buffers (§2.2, §7.3).
class GPUBufferManager {
public:
    explicit GPUBufferManager(GpuMemoryAllocator& allocator);
    ~GPUBufferManager();

    GPUBufferManager(const GPUBufferManager&) = delete;
    GPUBufferManager& operator=(const GPUBufferManager&) = delete;

    // ---- GPU SoA SSBO Management (§7.3) ----

    struct SSBOLayout {
        GpuAllocation gpu_bounds;          // AABB, 24B/obj
        GpuAllocation gpu_transforms;      // float4x4, 64B/obj
        GpuAllocation gpu_velocities;      // float3, 12B/obj
        GpuAllocation gpu_mesh_info;       // 16B/obj
        GpuAllocation gpu_material_ids;    // uint32, 4B/obj
        GpuAllocation gpu_lod_info;        // 32B/obj
        GpuAllocation gpu_visibility;      // uint32, 4B/obj
        GpuAllocation indirect_draw;       // DrawIndexedIndirectCommand, 20B/obj
        GpuAllocation draw_count;          // atomic uint32
    };

    /// Allocate GPU SoA buffers for `object_count` objects
    SSBOLayout allocate_soa_buffers(uint32_t object_count);

    /// Resize GPU SoA buffers
    void resize_soa_buffers(SSBOLayout& layout, uint32_t new_count);

    /// Free GPU SoA buffers
    void free_soa_buffers(SSBOLayout& layout);

    // ---- Mesh Pool (§11.2) ----

    struct MeshAllocation {
        GpuAllocation vertex_buffer;
        GpuAllocation index_buffer;
        uint32_t vertex_count;
        uint32_t index_count;
    };

    MeshAllocation allocate_mesh(size_t vertex_data_size, size_t index_data_size);
    void free_mesh(MeshAllocation& alloc);

    // ---- Instance Buffer (§11.2) ----

    GpuAllocation allocate_instance_data(size_t size);

    // ---- Staging (§5.6) ----

    /// Allocate staging buffer for CPU→GPU transfer
    GpuAllocation allocate_staging(size_t size);

    /// Reset per-frame ring buffers
    void reset_frame_buffers();

    // ---- Dirty Region Tracking (§5.6) ----

    struct DirtyRegion {
        uint32_t chunk_index;
        uint32_t start_object;
        uint32_t end_object;
    };

    void mark_dirty(uint32_t start, uint32_t end, uint32_t chunk_size = 16384);
    const std::vector<DirtyRegion>& dirty_regions() const { return dirty_regions_; }
    void clear_dirty() { dirty_regions_.clear(); }
    bool should_full_copy(uint32_t total_objects) const;

    // ---- Stats ----

    GpuMemoryAllocator::Stats get_stats() const;

private:
    GpuMemoryAllocator& allocator_;
    SSBOLayout          current_layout_;
    std::vector<DirtyRegion> dirty_regions_;
    uint32_t            dirty_chunk_count_ = 0;
};

} // namespace pictor
