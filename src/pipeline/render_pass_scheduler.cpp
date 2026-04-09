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
                // Shadow pass: render shadow-casting geometry into the shadow atlas.
                // Uses shadow-specific material variants (stripped to alpha test only).
                //
                // Vulkan implementation steps:
                //   1. Remap batches to shadow pass variants (strip unused features)
                //   2. Filter batches: only objects with CAST_SHADOW flag
                //   3. For each cascade (0..cascadeCount-1):
                //      a. Begin render pass (depth-only, shadow atlas layer as attachment)
                //      b. Set viewport/scissor to cascade resolution
                //      c. Bind shadow_depth pipeline
                //      d. Bind cascade's lightViewProj via push constant
                //      e. For each shadow batch:
                //         - Push model matrix, cascade index, materialFlags, alphaCutoff
                //         - Bind vertex/index buffers
                //         - If alpha test: bind albedo texture to set=1, binding=0
                //         - vkCmdDrawIndexed
                //      f. End render pass
                //   4. Transition shadow atlas to SHADER_READ_ONLY for fragment sampling
                {
                    auto shadow_batches = remap_batches_for_pass(batches, PassType::SHADOW);
                    // Filter out non-shadow-casting batches
                    // (materials without CAST_SHADOW feature will have zeroed shader key)
                    (void)shadow_batches;
                }
                break;

            case PassType::DEPTH_ONLY:
                // Record depth-only pre-pass commands
                break;

            case PassType::OPAQUE:
                // Record opaque geometry draw commands
                // Shadow map is bound as set=2 for sampling
                // Uses batches sorted front-to-back
                break;

            case PassType::TRANSPARENT:
                // Record transparent geometry draw commands
                // Shadow map is bound as set=2 for sampling
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
        const auto* variant = material_registry_->variant_for(
            static_cast<MaterialHandle>(batch.materialKey), pass_type);

        if (variant) {
            // For shadow pass: skip materials that don't cast shadows
            if (pass_type == PassType::SHADOW &&
                !(variant->features & MaterialFeature::CAST_SHADOW)) {
                continue;
            }

            rb.shaderKey   = variant->shader_key;
            rb.materialKey = variant->material_key;
        }

        remapped.push_back(rb);
    }

    return remapped;
}

} // namespace pictor
