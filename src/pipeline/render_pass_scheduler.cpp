#include "pictor/pipeline/render_pass_scheduler.h"

namespace pictor {

RenderPassScheduler::RenderPassScheduler(const PipelineProfileDef& profile)
    : pass_order_(profile.render_passes)
{
}

RenderPassScheduler::~RenderPassScheduler() = default;

void RenderPassScheduler::reconfigure(const PipelineProfileDef& profile) {
    // §8.4 step 5: Reconfigure pass order from new profile
    pass_order_ = profile.render_passes;
}

void RenderPassScheduler::register_custom_pass(ICustomRenderPass* pass) {
    custom_passes_.push_back(pass);
}

void RenderPassScheduler::execute(const BatchBuilder& batch_builder,
                                   const CullingSystem& culling,
                                   GPUDrivenPipeline* gpu_pipeline) {
    const auto& batches = batch_builder.batches();

    for (const auto& pass_def : pass_order_) {
        // Check for custom pass with matching name
        bool handled = false;
        for (auto* custom : custom_passes_) {
            if (pass_def.pass_name == custom->name()) {
                custom->execute(batches);
                handled = true;
                break;
            }
        }
        if (handled) continue;

        // Execute built-in pass types
        switch (pass_def.pass_type) {
            case PassType::COMPUTE:
                if (pass_def.gpu_driven_pass && gpu_pipeline) {
                    // GPU Driven compute passes are handled by GPUDrivenPipeline
                    // The pipeline dispatches compute shaders for:
                    // - ComputeUpdate, GPUCullPass, GPULODCompact
                }
                break;

            case PassType::SHADOW:
                // Record shadow map render commands for each cascade
                break;

            case PassType::DEPTH_ONLY:
                // Record depth-only pre-pass commands
                break;

            case PassType::OPAQUE:
                // Record opaque geometry draw commands
                // Uses batches sorted front-to-back
                break;

            case PassType::TRANSPARENT:
                // Record transparent geometry draw commands
                // Uses batches sorted back-to-front
                break;

            case PassType::POST_PROCESS:
                // Execute post-process stack (full-screen passes)
                break;

            case PassType::CUSTOM:
                // Unhandled custom pass (no matching ICustomRenderPass)
                break;
        }
    }
}

std::vector<RenderBatch> RenderPassScheduler::remap_batches_for_pass(
    const std::vector<RenderBatch>& batches,
    PassType pass_type) const
{
    if (!material_registry_) return batches;

    std::vector<RenderBatch> remapped;
    remapped.reserve(batches.size());

    for (const auto& batch : batches) {
        RenderBatch rb = batch;

        // Look up the pass-specific variant for this batch's material.
        // The batch carries a materialKey that was assigned at build time;
        // we need to resolve it via the MaterialHandle stored in the pool.
        // For now we use materialKey as handle lookup (direct mapping).
        const auto* variant = material_registry_->variant_for(
            static_cast<MaterialHandle>(batch.materialKey), pass_type);

        if (variant) {
            rb.shaderKey   = variant->shader_key;
            rb.materialKey = variant->material_key;
        }

        remapped.push_back(rb);
    }

    return remapped;
}

} // namespace pictor
