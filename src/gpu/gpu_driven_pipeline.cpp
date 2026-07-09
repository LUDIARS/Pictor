#include "pictor/gpu/gpu_driven_pipeline.h"

#include <cstdio>

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

    // SSBO プール枯渇時に initialized_=true のまま進めない (§7.1 サイレント失敗禁止)
    const bool all_valid = ssbo_layout_.gpu_bounds.valid &&
                           ssbo_layout_.gpu_transforms.valid &&
                           ssbo_layout_.gpu_velocities.valid &&
                           ssbo_layout_.gpu_mesh_info.valid &&
                           ssbo_layout_.gpu_material_ids.valid &&
                           ssbo_layout_.gpu_lod_info.valid &&
                           ssbo_layout_.gpu_visibility.valid &&
                           ssbo_layout_.indirect_draw.valid &&
                           ssbo_layout_.draw_count.valid;
    if (!all_valid) {
        std::fprintf(stderr,
                     "[GPUDrivenPipeline] initialize(%u): SSBO 確保に失敗 "
                     "(プール枯渇) — GPU Driven パスを無効のままにします\n",
                     max_objects);
        buffer_manager_.free_soa_buffers(ssbo_layout_);
        initialized_ = false;
        return;
    }

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

    // §7.2 の 4 ステップ (Compute Update → Compute Cull → LOD Select →
    // Compact + Indirect Draw) は **compute dispatch 未実装** (Phase 4)。
    // §7.1 サイレント no-op 禁止に従い、 初回のみ明示 warn を出す。
    if (!warned_dispatch_unimplemented_) {
        std::fprintf(stderr,
                     "[GPUDrivenPipeline] execute(): compute dispatch は未実装 "
                     "(Phase 4) — GPU cull / indirect draw は発行されません "
                     "(stats.visible_objects / draw_calls は未計測 = 0)\n");
        warned_dispatch_unimplemented_ = true;
    }

    // 旧実装は visible_objects = object_count / draw_calls = 1 の推定値を
    // 返していた (review/2026-06-11 D-2 虚偽統計)。 GPU readback 実装まで
    // 「未計測 = 0」に固定し、 偽値を統計 API に流さない。
    stats_.visible_objects = 0;
    stats_.draw_calls      = 0;
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
