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

#ifdef PICTOR_HAS_VULKAN

void RenderPassScheduler::execute_compiled(VkCommandBuffer cmd,
                                            uint32_t        flight_index,
                                            uint32_t        image_index,
                                            const PassRecordFn& record)
{
    // Hot path: flat seq-iterate the compiled graph. Per-pass `VkHandle`
    // values are pre-resolved; the host record callback is the *only* place
    // we leave the scheduler per pass. Phase 3 §3.5 hot-path invariant.
    for (const CompiledPass& cp : compiled_.passes) {
        const uint32_t slot = cp.is_swapchain() ? image_index : flight_index;
        const VkFramebuffer fb = (slot < cp.framebuffer_count)
                                     ? cp.framebuffers[slot]
                                     : VK_NULL_HANDLE;

        // Bind pre-built input descriptors when present (Phase 4 will give
        // each pass its own layout; right now an empty set is harmless).
        const VkDescriptorSet input_set = cp.input_sets[
                std::min<uint32_t>(flight_index,
                                   static_cast<uint32_t>(cp.input_sets.size() - 1))];
        if (input_set != VK_NULL_HANDLE) {
            // Phase 4: parameterize set index / pipeline layout.
            // Phase 3 stays no-op-friendly for hosts that haven't wired
            // input_textures yet.
        }

        if (cp.is_compute()) {
            // Compute: no render pass, host issues vkCmdDispatch in `record`.
            if (record) record(cmd, cp, flight_index, image_index);
            continue;
        }

        if (cp.is_host_recorded()) {
            // host_recorded: PostProcess chain 等、 内部で複数 sub-render-pass
            // を持つ pass。 execute_compiled は Begin/End / BindPipeline を
            // 一切発行せず、 host の record callback に command 記録を委ねる。
            if (record) record(cmd, cp, flight_index, image_index);
            continue;
        }

        if (cp.render_pass == VK_NULL_HANDLE || fb == VK_NULL_HANDLE) {
            // Missing GPU resources — host record can still observe the
            // pass (e.g. for tagging) but BeginRenderPass would assert.
            if (record) record(cmd, cp, flight_index, image_index);
            continue;
        }

        VkRenderPassBeginInfo bi{};
        bi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        bi.renderPass        = cp.render_pass;
        bi.framebuffer       = fb;
        bi.renderArea        = cp.render_area;
        bi.clearValueCount   = cp.clear_count;
        bi.pClearValues      = cp.clear_count ? cp.clear_values.data() : nullptr;

        vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);

        if (cp.pipeline != VK_NULL_HANDLE) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cp.pipeline);
        }

        if (record) record(cmd, cp, flight_index, image_index);

        vkCmdEndRenderPass(cmd);
    }
}

#endif // PICTOR_HAS_VULKAN

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
