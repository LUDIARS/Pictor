#include "pictor/gpu/gpu_buffer_manager.h"

namespace pictor {

GPUBufferManager::GPUBufferManager(GpuMemoryAllocator& allocator)
    : allocator_(allocator)
{
}

GPUBufferManager::~GPUBufferManager() = default;

GPUBufferManager::SSBOLayout GPUBufferManager::allocate_soa_buffers(uint32_t object_count) {
    SSBOLayout layout;

    // §7.3: GPU-side SoA buffer layout
    layout.gpu_bounds      = allocator_.allocate_ssbo(object_count * 24);   // AABB 24B
    layout.gpu_transforms  = allocator_.allocate_ssbo(object_count * 64);   // float4x4 64B
    layout.gpu_velocities  = allocator_.allocate_ssbo(object_count * 12);   // float3 12B
    layout.gpu_mesh_info   = allocator_.allocate_ssbo(object_count * 16);   // 16B
    layout.gpu_material_ids = allocator_.allocate_ssbo(object_count * 4);   // uint32 4B
    layout.gpu_lod_info    = allocator_.allocate_ssbo(object_count * 32);   // 32B
    layout.gpu_visibility  = allocator_.allocate_ssbo(object_count * 4);    // uint32 4B

    // Indirect draw buffer: DrawIndexedIndirectCommand 20B per object
    layout.indirect_draw   = allocator_.allocate_ssbo(object_count * 20);

    // Draw count buffer: single atomic uint32
    layout.draw_count      = allocator_.allocate_ssbo(sizeof(uint32_t));

    current_layout_ = layout;
    return layout;
}

void GPUBufferManager::resize_soa_buffers(SSBOLayout& layout, uint32_t new_count) {
    // Free old buffers
    free_soa_buffers(layout);
    // Allocate new
    layout = allocate_soa_buffers(new_count);
}

void GPUBufferManager::free_soa_buffers(SSBOLayout& layout) {
    allocator_.free(layout.gpu_bounds);
    allocator_.free(layout.gpu_transforms);
    allocator_.free(layout.gpu_velocities);
    allocator_.free(layout.gpu_mesh_info);
    allocator_.free(layout.gpu_material_ids);
    allocator_.free(layout.gpu_lod_info);
    allocator_.free(layout.gpu_visibility);
    allocator_.free(layout.indirect_draw);
    allocator_.free(layout.draw_count);

    layout = {};
}

GPUBufferManager::MeshAllocation GPUBufferManager::allocate_mesh(
    size_t vertex_data_size, size_t index_data_size) {
    MeshAllocation alloc;
    alloc.vertex_buffer = allocator_.allocate_mesh(vertex_data_size);
    alloc.index_buffer = allocator_.allocate_mesh(index_data_size);
    alloc.vertex_count = 0;
    alloc.index_count = 0;
    return alloc;
}

void GPUBufferManager::free_mesh(MeshAllocation& alloc) {
    allocator_.free(alloc.vertex_buffer);
    allocator_.free(alloc.index_buffer);
    alloc = {};
}

GpuAllocation GPUBufferManager::allocate_instance_data(size_t size) {
    return allocator_.allocate_instance(size);
}

GpuAllocation GPUBufferManager::allocate_staging(size_t size) {
    return allocator_.allocate_staging(size);
}

void GPUBufferManager::reset_frame_buffers() {
    allocator_.reset_ring_buffers();
    dirty_regions_.clear();
    dirty_chunk_count_ = 0;
}

void GPUBufferManager::mark_dirty(uint32_t start, uint32_t end, uint32_t chunk_size) {
    // §5.6: Dirty Region Tracking
    uint32_t start_chunk = start / chunk_size;
    uint32_t end_chunk = (end + chunk_size - 1) / chunk_size;

    for (uint32_t c = start_chunk; c < end_chunk; ++c) {
        // Check if chunk already marked
        bool found = false;
        for (const auto& region : dirty_regions_) {
            if (region.chunk_index == c) {
                found = true;
                break;
            }
        }
        if (!found) {
            dirty_regions_.push_back({
                c,
                c * chunk_size,
                std::min((c + 1) * chunk_size, end)
            });
            ++dirty_chunk_count_;
        }
    }
}

bool GPUBufferManager::should_full_copy(uint32_t total_objects) const {
    // §5.6: If most chunks are dirty, skip dirty tracking overhead
    uint32_t total_chunks = (total_objects + 16383) / 16384;
    return dirty_chunk_count_ > total_chunks / 2;
}

GpuMemoryAllocator::Stats GPUBufferManager::get_stats() const {
    return allocator_.get_stats();
}

} // namespace pictor
