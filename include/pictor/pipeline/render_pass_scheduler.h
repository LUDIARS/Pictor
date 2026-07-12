#pragma once

#include "pictor/core/types.h"
#include "pictor/pipeline/pipeline_profile.h"
#include "pictor/pipeline/compiled_graph.h"
#include "pictor/batch/batch_builder.h"
#include "pictor/culling/culling_system.h"
#include "pictor/gpu/gpu_driven_pipeline.h"
#include "pictor/material/base_material_builder.h"
#include <vector>
#include <functional>
#include <string_view>

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
    /// Consumed by the host-side record path (`CompiledBatchRecorder` が
    /// batch 単位でインライン解決する) — `material_registry()` で参照する。
    void set_material_registry(const MaterialRegistry* registry) { material_registry_ = registry; }
    const MaterialRegistry* material_registry() const { return material_registry_; }

    /// Managed 経路のパス巡回 — custom pass (`ICustomRenderPass`) の実行のみ。
    ///
    /// built-in pass (SHADOW/OPAQUE/...) の描画コマンド発行は host-driven の
    /// `execute_compiled()` が担う (spec/pipeline-system-b-config.md §1.2 —
    /// コマンドバッファ / メッシュ VkBuffer は host 所有)。 compiled graph
    /// 未設置のまま built-in pass に遭遇した場合は「動いて見えて何もしない」
    /// を避けるため 1 回だけ明示 warn を出す (§7.1 サイレント no-op 禁止)。
    void execute(const BatchBuilder& batch_builder,
                 const CullingSystem& culling,
                 GPUDrivenPipeline* gpu_pipeline);

    /// Get the ordered pass list
    const std::vector<RenderPassDef>& pass_order() const { return pass_order_; }

    /// Statistics
    uint32_t pass_count() const { return static_cast<uint32_t>(pass_order_.size()); }

    /// Resolve a pass name outside the hot path. The returned ID matches the
    /// CompiledPass::pass_id produced by PipelineCompiler for the same profile.
    uint16_t pass_id_of(std::string_view pass_name) const;

    // ---- Phase 3: CompiledGraph hot path (`spec/pipeline-system-b-config.md` §3.5) ----

    /// Install a pre-compiled graph (produced by `PipelineCompiler::compile()`).
    /// Ownership transferred — scheduler will not call `graph.shutdown()`,
    /// host is responsible for that on device tear-down. Set to an empty
    /// graph to disengage and fall back to the old `execute()` path.
    void set_compiled_graph(CompiledGraph graph);

    /// True if a non-empty CompiledGraph is installed.
    bool has_compiled_graph() const { return !compiled_.passes.empty(); }

    /// Take back ownership of the installed graph (scheduler は空 graph に
    /// 戻る)。 再 compile / device 破棄前に `CompiledGraph::shutdown()` を
    /// 呼ぶのは受け取った側の責務 (`CompiledPathDriver` が正規の呼び手)。
    CompiledGraph take_compiled_graph() {
        CompiledGraph g = std::move(compiled_);
        compiled_ = CompiledGraph{};
        return g;
    }

    /// Read-only access to the installed graph (for diagnostics / KS-side
    /// custom recording).
    const CompiledGraph& compiled_graph() const { return compiled_; }

#ifdef PICTOR_HAS_VULKAN
    /// Per-pass record callback signature used by `execute_compiled()`.
    ///
    /// Called inside `vkCmdBeginRenderPass` / `vkCmdEndRenderPass` (or
    /// instead of them for compute passes). The scheduler hands the host
    /// the CompiledPass + the current frame's command buffer and frame
    /// indices, leaving the actual `vkCmdDraw*` work to the host (which
    /// owns the pipelines / vertex buffers / RenderBatch source-of-truth).
    using PassRecordFn = std::function<void(VkCommandBuffer cmd,
                                            const CompiledPass& cp,
                                            uint32_t flight_index,
                                            uint32_t image_index)>;

    /// Install/replace a pass-specific recorder. The table is keyed directly
    /// by CompiledPass::pass_id; no name lookup occurs during a frame.
    bool set_pass_record_callback(uint16_t pass_id, PassRecordFn record);
    void clear_pass_record_callbacks();

    /// Hot-path: iterate the compiled graph, BeginRenderPass / record /
    /// EndRenderPass. `record` is invoked once per pass *inside* the render
    /// pass scope (compute passes skip Begin/End and just call `record`).
    ///
    /// 不変条件 (§3.5):
    ///   - hot path に `std::string` / `unordered_map` ゼロ
    ///   - per-pass の VkHandle は CompiledPass 直値
    ///   - 全 pass 共通の framebuffer 選択ロジックは 1 分岐
    ///     (`cp.is_swapchain() ? image_index : flight_index`)
    void execute_compiled(VkCommandBuffer cmd,
                           uint32_t        flight_index,
                           uint32_t        image_index,
                           const PassRecordFn& record);
#endif // PICTOR_HAS_VULKAN

private:
    std::vector<RenderPassDef>       pass_order_;
    std::vector<ICustomRenderPass*>  custom_passes_;
    const MaterialRegistry*          material_registry_ = nullptr;
    CompiledGraph                    compiled_;
    /// execute() の「compiled graph 未設置で built-in pass に遭遇」warn を
    /// 毎フレーム出さないための 1 回きりフラグ (§7.1)。
    bool                             warned_unwired_builtin_ = false;
#ifdef PICTOR_HAS_VULKAN
    std::vector<PassRecordFn>        pass_record_callbacks_;

    void record_pass_(VkCommandBuffer cmd, const CompiledPass& cp,
                      uint32_t flight_index, uint32_t image_index,
                      const PassRecordFn& fallback);
#endif
};

} // namespace pictor
