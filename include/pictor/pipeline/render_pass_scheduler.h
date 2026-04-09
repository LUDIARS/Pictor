#pragma once

#include "pictor/core/types.h"
#include "pictor/pipeline/pipeline_profile.h"
#include "pictor/batch/batch_builder.h"
#include "pictor/culling/culling_system.h"
#include "pictor/gpu/gpu_driven_pipeline.h"
#include "pictor/material/base_material_builder.h"
#include <vector>
#include <functional>

namespace pictor {

/// Custom render pass interface (§12.2)
class ICustomRenderPass {
public:
    virtual ~ICustomRenderPass() = default;

    virtual const char* name() const = 0;
    virtual PassType type() const = 0;

    /// Declare which SoA streams are needed (prefetch hint)
    virtual std::vector<std::string> required_streams() const { return {}; }

    /// Execute the custom pass
    virtual void execute(const std::vector<RenderBatch>& batches) = 0;
};

/// Render pass scheduler: determines pass execution order from profile (§2.2, §8.4).
class RenderPassScheduler {
public:
    explicit RenderPassScheduler(const PipelineProfileDef& profile);
    ~RenderPassScheduler();

    /// Reconfigure from a new profile (§8.4 step 5)
    void reconfigure(const PipelineProfileDef& profile);

    /// Register a custom render pass (§12.2)
    void register_custom_pass(ICustomRenderPass* pass);

    /// Set material registry for pass-specific variant resolution.
    /// When set, the scheduler resolves per-pass shader/material keys
    /// from the registry, stripping unused features per pass.
    void set_material_registry(const MaterialRegistry* registry) { material_registry_ = registry; }

    /// Execute all render passes in order
    void execute(const BatchBuilder& batch_builder,
                 const CullingSystem& culling,
                 GPUDrivenPipeline* gpu_pipeline);

    /// Get the ordered pass list
    const std::vector<RenderPassDef>& pass_order() const { return pass_order_; }

    /// Statistics
    uint32_t pass_count() const { return static_cast<uint32_t>(pass_order_.size()); }

private:
    std::vector<RenderPassDef>       pass_order_;
    std::vector<ICustomRenderPass*>  custom_passes_;
    const MaterialRegistry*          material_registry_ = nullptr;

    /// Remap batch keys using pass-specific material variants.
    /// Returns a new batch list with shader/material keys replaced by
    /// the variant appropriate for `pass_type`.
    std::vector<RenderBatch> remap_batches_for_pass(
        const std::vector<RenderBatch>& batches,
        PassType pass_type) const;
};

} // namespace pictor
