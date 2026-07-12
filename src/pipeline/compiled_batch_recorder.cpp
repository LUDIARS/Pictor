#include "pictor/pipeline/compiled_batch_recorder.h"

#include <cstdio>

namespace pictor {

#ifdef PICTOR_HAS_VULKAN

void CompiledBatchRecorder::record(VkCommandBuffer cmd, const CompiledPass& cp,
                                   uint32_t /*flight_index*/,
                                   uint32_t /*image_index*/)
{
    const PassType pass_type = static_cast<PassType>(cp.pass_type);

    switch (pass_type) {
        case PassType::OPAQUE:
        case PassType::TRANSPARENT:
            record_batches_(cmd, cp, pass_type);
            break;

        case PassType::SHADOW:
            // 未実装: shadow atlas / cascade ごとの depth-only 記録は
            // per-cascade lightViewProj と shadow 専用 pipeline が要る
            // (GILightingSystem 側の資産が host-driven 化されるまで保留)。
            // §7.1: 無言 skip せず 1 回だけ warn + stats へ計上する。
            if (!warned_shadow_) {
                std::fprintf(stderr,
                             "[CompiledBatchRecorder] SHADOW pass '%s' は未実装 "
                             "— 描画コマンドを発行しません (stats.passes_unimplemented)\n",
                             cp.debug_name ? cp.debug_name : "?");
                warned_shadow_ = true;
            }
            stats_.passes_unimplemented++;
            break;

        case PassType::DEPTH_ONLY:
            // 未実装: depth pre-pass は depth-only pipeline 変種の解決が
            // IBatchGpuSource 契約にまだ無い (pass_type 引数で拡張可能)。
            if (!warned_depth_only_) {
                std::fprintf(stderr,
                             "[CompiledBatchRecorder] DEPTH_ONLY pass '%s' は未実装 "
                             "— 描画コマンドを発行しません (stats.passes_unimplemented)\n",
                             cp.debug_name ? cp.debug_name : "?");
                warned_depth_only_ = true;
            }
            stats_.passes_unimplemented++;
            break;

        case PassType::COMPUTE:
        case PassType::POST_PROCESS:
        case PassType::CUSTOM:
        default:
            // record 対象外: COMPUTE dispatch / PostProcessPipeline chain /
            // custom pass は host が自前の PassRecordFn で記録する契約
            // (compiled_graph.h FLAG_IS_HOST_RECORDED 参照)。 これらを本
            // recorder 一本で受けている構成は配線漏れの可能性があるため
            // 1 回だけ知らせる (§7.1)。
            if (!warned_not_recorded_) {
                std::fprintf(stderr,
                             "[CompiledBatchRecorder] pass '%s' (type=%u) は本 "
                             "recorder の対象外 — host 側 PassRecordFn で記録して"
                             "ください (stats.passes_not_recorded)\n",
                             cp.debug_name ? cp.debug_name : "?",
                             static_cast<unsigned>(cp.pass_type));
                warned_not_recorded_ = true;
            }
            stats_.passes_not_recorded++;
            break;
    }
}

void CompiledBatchRecorder::record_batches_(VkCommandBuffer cmd,
                                            const CompiledPass& cp,
                                            PassType pass_type)
{
    if (!batches_ || batches_->empty()) return;

    // viewport / scissor は dynamic state 前提で render_area から張る
    // (simple_renderer.cpp と同じ手順)。 extent 0 = attachment 未実体化
    // (headless) なので何も発行しない。
    const bool has_area = cp.render_area.extent.width  > 0 &&
                          cp.render_area.extent.height > 0;
    if (has_area) {
        VkViewport viewport{};
        viewport.x        = static_cast<float>(cp.render_area.offset.x);
        viewport.y        = static_cast<float>(cp.render_area.offset.y);
        viewport.width    = static_cast<float>(cp.render_area.extent.width);
        viewport.height   = static_cast<float>(cp.render_area.extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &cp.render_area);
    }

    // BatchBuilder の sortKey 順 (shader/material グルーピング済み) を尊重して
    // seq-iterate する。 per-pass の再ソート (cp.sort_mode) は per-frame alloc
    // なしでは組めないため host 拡張に譲る (M-2 の轍を踏まない)。
    VkPipeline bound_pipeline = VK_NULL_HANDLE;

    for (const RenderBatch& batch : *batches_) {
        const uint32_t batch_filter = RenderBatchFilter::from_transparency(
            batch.transparency);
        if ((cp.filter_mask & batch_filter) == 0) {
            stats_.batches_filtered++;
            continue;
        }

        // pass 別 material variant 解決 (旧 remap_batches_for_pass の
        // vector 確保を廃し、 バッチ単位のインライン解決に置換 — M-2)。
        uint64_t shader_key = batch.shaderKey;
        if (material_registry_) {
            const PassMaterialVariant* variant = material_registry_->variant_for(
                static_cast<MaterialHandle>(batch.materialKey), pass_type);
            if (variant) shader_key = variant->shader_key;
        }

        BatchGpuResources res;
        if (!gpu_source_.resolve(batch, shader_key, pass_type, res) ||
            res.pipeline      == VK_NULL_HANDLE ||
            res.vertex_buffer == VK_NULL_HANDLE ||
            res.index_buffer  == VK_NULL_HANDLE ||
            res.index_count   == 0) {
            // host がまだ GPU 実体を持たないバッチ (アップロード前など)。
            // 計上して先へ — フレーム全体は止めない。
            stats_.batches_skipped++;
            continue;
        }

        // render_area なし (headless) で GPU 実体だけある場合も draw は
        // 発行しない — viewport 未設定の dynamic state pipeline で draw
        // すると未定義動作。 skip として計上する。
        if (!has_area) {
            stats_.batches_skipped++;
            continue;
        }

        // cp.pipeline は profile の shader_override による pass 単位の直値
        // (compile 済み)。 ある場合はバッチ解決より優先する。
        const VkPipeline pipeline =
            (cp.pipeline != VK_NULL_HANDLE) ? cp.pipeline : res.pipeline;
        if (pipeline != bound_pipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bound_pipeline = pipeline;
        }

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &res.vertex_buffer, &offset);
        vkCmdBindIndexBuffer(cmd, res.index_buffer, 0, res.index_type);

        // batch.count = インスタンス数、 batch.startIndex = sorted_indices 上の
        // 先頭 (§6.1 instanced draw)。 firstInstance に渡し、 シェーダ側は
        // gl_InstanceIndex で instance SSBO を引く (simple_renderer と同型)。
        vkCmdDrawIndexed(cmd, res.index_count, batch.count, 0, 0, batch.startIndex);

        stats_.draw_calls++;
        stats_.instances += batch.count;
        stats_.triangles += static_cast<uint64_t>(res.index_count / 3u) * batch.count;
    }
}

#endif // PICTOR_HAS_VULKAN

} // namespace pictor
