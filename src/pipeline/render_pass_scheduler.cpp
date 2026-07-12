#include "pictor/pipeline/render_pass_scheduler.h"

#include <cstdio>
#include <utility>

namespace pictor {

RenderPassScheduler::RenderPassScheduler(const PipelineProfileDef& profile)
    : pass_order_(profile.render_passes)
{
#ifdef PICTOR_HAS_VULKAN
    pass_record_callbacks_.resize(pass_order_.size());
#endif
}

RenderPassScheduler::~RenderPassScheduler() = default;

void RenderPassScheduler::reconfigure(const PipelineProfileDef& profile) {
    // §8.4 step 5: Reconfigure pass order from new profile
    pass_order_ = profile.render_passes;
#ifdef PICTOR_HAS_VULKAN
    // IDs are scoped to the compiled profile. Retaining callbacks across a
    // different pass layout could silently dispatch work to the wrong pass.
    pass_record_callbacks_.clear();
    pass_record_callbacks_.resize(pass_order_.size());
#endif
}

uint16_t RenderPassScheduler::pass_id_of(std::string_view pass_name) const {
    // Once compiled, use the graph as the source of truth. This keeps lookup
    // correct even for callers that recompile directly through CompiledPathDriver.
    for (const CompiledPass& pass : compiled_.passes) {
        if (pass.debug_name && pass_name == pass.debug_name) return pass.pass_id;
    }

    for (size_t i = 0; i < pass_order_.size(); ++i) {
        if (pass_order_[i].pass_name == pass_name) {
            if (i >= CompiledPass::INVALID_PASS_ID) return CompiledPass::INVALID_PASS_ID;
            return static_cast<uint16_t>(i);
        }
    }
    return CompiledPass::INVALID_PASS_ID;
}

void RenderPassScheduler::set_compiled_graph(CompiledGraph graph) {
    compiled_ = std::move(graph);
#ifdef PICTOR_HAS_VULKAN
    // pass_id is scoped to a compiled graph. Never carry callbacks into a
    // rebuilt graph where the same numeric ID may name another pass.
    pass_record_callbacks_.clear();
    pass_record_callbacks_.resize(compiled_.passes.size());
#endif
}

void RenderPassScheduler::register_custom_pass(ICustomRenderPass* pass) {
    custom_passes_.push_back(pass);
}

void RenderPassScheduler::execute(const BatchBuilder& batch_builder,
                                   const CullingSystem& /*culling*/,
                                   GPUDrivenPipeline* gpu_pipeline) {
    const auto& batches = batch_builder.batches();

    for (const auto& pass_def : pass_order_) {
        // Registered custom passes run here — the only work the managed
        // path executes itself (host-provided CPU-side logic).
        bool handled = false;
        for (auto* custom : custom_passes_) {
            if (pass_def.pass_name == custom->name()) {
                custom->execute(batches);
                handled = true;
                break;
            }
        }
        if (handled) continue;

        // gpu_driven な COMPUTE pass は GPUDrivenPipeline::execute() が
        // フレームループ側で別途担当する (pictor_renderer.cpp §7.2)。
        if (pass_def.pass_type == PassType::COMPUTE &&
            pass_def.gpu_driven_pass && gpu_pipeline) {
            continue;
        }

        // built-in pass の描画コマンド発行は managed 経路では行わない —
        // コマンドバッファ / メッシュ VkBuffer / VkPipeline は host 所有
        // (spec/pipeline-system-b-config.md §1.2)。 compiled graph が
        // 設置済みなら host が execute_compiled() で同じ pass 構成を実描画
        // しているので、 ここは委譲済みとして素通りしてよい。
        if (has_compiled_graph()) continue;

        // compiled graph 未設置 = 実描画経路がどこにも無い。 旧実装は空 case
        // で無言 no-op だった (review/2026-06-11 D-2 「動いているように見えて
        // 何もしない」)。 §7.1 に従い 1 回だけ明示 warn する。
        if (!warned_unwired_builtin_) {
            std::fprintf(stderr,
                         "[RenderPassScheduler] execute(): built-in pass '%s' は "
                         "managed 経路では描画されません。 host 側で "
                         "PipelineCompiler::compile() (PictorRenderer::"
                         "compile_render_graph) → execute_compiled() を配線して"
                         "ください\n",
                         pass_def.pass_name.c_str());
            warned_unwired_builtin_ = true;
        }
    }
}

#ifdef PICTOR_HAS_VULKAN

bool RenderPassScheduler::set_pass_record_callback(uint16_t pass_id,
                                                    PassRecordFn record) {
    if (pass_id == CompiledPass::INVALID_PASS_ID ||
        pass_id >= pass_record_callbacks_.size()) {
        return false;
    }
    pass_record_callbacks_[pass_id] = std::move(record);
    return true;
}

void RenderPassScheduler::clear_pass_record_callbacks() {
    for (PassRecordFn& record : pass_record_callbacks_) record = PassRecordFn{};
}

void RenderPassScheduler::record_pass_(VkCommandBuffer cmd,
                                       const CompiledPass& cp,
                                       uint32_t flight_index,
                                       uint32_t image_index,
                                       const PassRecordFn& fallback) {
    if (cp.pass_id < pass_record_callbacks_.size()) {
        const PassRecordFn& record = pass_record_callbacks_[cp.pass_id];
        if (record) {
            record(cmd, cp, flight_index, image_index);
            return;
        }
    }
    if (fallback) fallback(cmd, cp, flight_index, image_index);
}

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
            record_pass_(cmd, cp, flight_index, image_index, record);
            continue;
        }

        if (cp.is_host_recorded()) {
            // host_recorded: PostProcess chain 等、 内部で複数 sub-render-pass
            // を持つ pass。 execute_compiled は Begin/End / BindPipeline を
            // 一切発行せず、 host の record callback に command 記録を委ねる。
            record_pass_(cmd, cp, flight_index, image_index, record);
            continue;
        }

        if (cp.render_pass == VK_NULL_HANDLE || fb == VK_NULL_HANDLE) {
            // Missing GPU resources — host record can still observe the
            // pass (e.g. for tagging) but BeginRenderPass would assert.
            record_pass_(cmd, cp, flight_index, image_index, record);
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

        record_pass_(cmd, cp, flight_index, image_index, record);

        vkCmdEndRenderPass(cmd);
    }
}

#endif // PICTOR_HAS_VULKAN

// remap_batches_for_pass は削除 (review/2026-06-11 M-2): pass ごとの
// std::vector 新規確保を伴う旧 managed 経路専用ヘルパで、 呼び手が D-2 の
// 空スタブしか無かった。 pass 別 material variant 解決は
// `CompiledBatchRecorder` がバッチ単位のインライン解決 (alloc なし) で行う。

} // namespace pictor
