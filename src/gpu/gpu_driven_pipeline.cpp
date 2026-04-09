#include "pictor/gpu/gpu_driven_pipeline.h"

namespace pictor {

GPUDrivenPipeline::GPUDrivenPipeline(GPUBufferManager& buffer_manager,
                                     SceneRegistry& registry)
    : GPUDrivenPipeline(buffer_manager, registry, GPUDrivenConfig{})
{}

GPUDrivenPipeline::GPUDrivenPipeline(GPUBufferManager& buffer_manager,
                                     SceneRegistry& registry,
                                     const GPUDrivenConfig& config)
    : buffer_manager_(buffer_manager)
    , registry_(registry)
    , config_(config)
{
}

GPUDrivenPipeline::~GPUDrivenPipeline() {
    if (initialized_) {
        buffer_manager_.free_soa_buffers(ssbo_layout_);
    }
}

void GPUDrivenPipeline::initialize(uint32_t max_objects) {
    if (initialized_) {
        buffer_manager_.free_soa_buffers(ssbo_layout_);
    }

    // Allocate GPU SoA buffers (§7.3)
    ssbo_layout_ = buffer_manager_.allocate_soa_buffers(max_objects);

    stats_.total_objects = max_objects;
    stats_.workgroups = calculate_workgroups(max_objects);
    initialized_ = true;
}

void GPUDrivenPipeline::execute(const Frustum& frustum,
                                 const UpdateScheduler::ComputeUpdateParams& params) {
    if (!initialized_) return;

    uint32_t object_count = registry_.gpu_driven_pool().count();
    if (object_count == 0) return;

    uint32_t workgroups = calculate_workgroups(object_count);
    stats_.workgroups = workgroups;
    stats_.total_objects = object_count;

    // §7.2: GPU Driven Pipeline Steps
    // All steps are Compute Shader dispatches — recorded as GPU commands.
    // Actual Vulkan dispatch calls would go here in a real implementation.

    // Step 1: Compute Update (§5.4, §7.2)
    // dispatch(workgroups, 1, 1) — updates gpu_transforms and gpu_bounds
    // Input: gpu_velocities, gpu_update_params (uniform)
    // Output: gpu_transforms, gpu_bounds

    // Step 2: Compute Cull (§7.2)
    // Frustum culling + Hi-Z occlusion culling
    // Input: gpu_bounds
    // Output: gpu_visibility

    // Step 3: LOD Select (§7.2)
    // Camera distance-based LOD selection
    // Input: gpu_transforms, gpu_lod_info
    // Output: gpu_visibility (LOD level stored)

    // Step 4: Compact + Indirect Draw (§7.2)
    // Compact visible objects and generate DrawIndexedIndirectCommand
    // Input: gpu_visibility, gpu_mesh_info
    // Output: IndirectDrawBuffer, DrawCountBuffer

    // In a real Vulkan implementation, these would be:
    // vkCmdDispatch(cmd, workgroups, 1, 1) for each step
    // with appropriate barriers between steps

    // Estimate visible count (actual would come from GPU readback)
    stats_.visible_objects = object_count; // placeholder
    stats_.draw_calls = 1; // single indirect draw call
}

void GPUDrivenPipeline::upload_initial_data(const ObjectPool& pool) {
    if (!initialized_) return;

    // Upload initial SoA data from CPU pools to GPU SSBOs
    // In a real implementation, this would use staging buffers
    // and vkCmdCopyBuffer to transfer data to device-local memory

    stats_.total_objects = pool.count();
}

uint32_t GPUDrivenPipeline::calculate_workgroups(uint32_t count) const {
    // §14.3: ceil(count / workgroup_size)
    return (count + config_.workgroup_size - 1) / config_.workgroup_size;
}

} // namespace pictor
