#pragma once

/// CompiledBatchRecorder — CompiledGraph の pass record callback 既定実装。
///
/// `RenderPassScheduler::execute_compiled()` の `PassRecordFn` として動き、
/// `BatchBuilder` が組んだ `RenderBatch` 列を実 `vkCmd*` へ落とす
/// (`simple_renderer.cpp` の record パターン踏襲)。 メッシュ VkBuffer /
/// VkPipeline は Pictor が所有しない (host 所有 — `rendering-extensibility-design.md`
/// §2 「実 vkCmdBindPipeline はホスト責務」) ため、 解決は `IBatchGpuSource`
/// 抽象を経由する (DIP: consumer に Vulkan 実体の置き場所を強制しない)。
///
/// 未実装 pass (SHADOW / DEPTH_ONLY) と record 対象外 pass (COMPUTE /
/// POST_PROCESS / CUSTOM / host_recorded) は §7.1 に従い無言 skip しない —
/// 種別ごとに 1 回だけ warn を出し、 stats に計上する。

// pipeline_profile.h precedes <vulkan/vulkan.h> — see render_pass_registry.h.
#include "pictor/pipeline/compiled_graph.h"
#include "pictor/core/types.h"
#include "pictor/material/base_material_builder.h"

#include <cstdint>
#include <vector>

namespace pictor {

#ifdef PICTOR_HAS_VULKAN

/// 1 バッチ分の GPU 実体 (host 所有ハンドルの借用ビュー)。
struct BatchGpuResources {
    VkPipeline  pipeline      = VK_NULL_HANDLE;
    VkBuffer    vertex_buffer = VK_NULL_HANDLE;
    VkBuffer    index_buffer  = VK_NULL_HANDLE;
    VkIndexType index_type    = VK_INDEX_TYPE_UINT32;
    uint32_t    index_count   = 0;
};

/// Host 側の GPU 実体解決 seam。 `shader_key` は material variant 解決後の
/// pass-specific キー (recorder が MaterialRegistry を引いた結果)。
/// 解決できないバッチは false を返す — recorder は skip を stats に計上する
/// ので、 host は「まだアップロードしていないメッシュ」を安全に落とせる。
class IBatchGpuSource {
public:
    virtual ~IBatchGpuSource() = default;

    virtual bool resolve(const RenderBatch& batch,
                         uint64_t           shader_key,
                         PassType           pass_type,
                         BatchGpuResources& out) = 0;
};

class CompiledBatchRecorder {
public:
    /// per-frame 実測統計 (§13.5 — 虚偽値を返さないための実測源)。
    struct Stats {
        uint32_t draw_calls          = 0;  ///< 発行した vkCmdDrawIndexed 数
        uint32_t instances           = 0;  ///< 描画インスタンス総数
        uint64_t triangles           = 0;  ///< index_count/3 × instance の総和
        uint32_t batches_skipped     = 0;  ///< GPU 実体未解決で skip したバッチ
        uint32_t passes_unimplemented = 0; ///< SHADOW/DEPTH_ONLY (未実装) の遭遇数
        uint32_t passes_not_recorded = 0;  ///< COMPUTE/POST_PROCESS 等 record 対象外
    };

    /// `gpu_source` は借用 (host 所有)。 `material_registry` は null 可 —
    /// null なら batch の shaderKey をそのまま使う (variant 解決なし)。
    explicit CompiledBatchRecorder(IBatchGpuSource& gpu_source,
                                   const MaterialRegistry* material_registry = nullptr)
        : gpu_source_(gpu_source)
        , material_registry_(material_registry)
    {}

    /// フレーム毎: 描画対象バッチ列を差し替え、 統計をリセットする。
    /// `batches` は借用 — execute_compiled の間、 呼び出し側が生存保証する。
    void begin_frame(const std::vector<RenderBatch>* batches) {
        batches_ = batches;
        stats_   = Stats{};
    }

    /// PassRecordFn 本体。 render pass scope 内 (execute_compiled が
    /// Begin/End を発行済み) で呼ばれる前提。
    void record(VkCommandBuffer cmd, const CompiledPass& cp,
                uint32_t flight_index, uint32_t image_index);

    const Stats& stats() const { return stats_; }

private:
    void record_batches_(VkCommandBuffer cmd, const CompiledPass& cp,
                         PassType pass_type);

    IBatchGpuSource&                gpu_source_;
    const MaterialRegistry*         material_registry_ = nullptr;
    const std::vector<RenderBatch>* batches_           = nullptr;
    Stats                           stats_;

    // §7.1: 未実装/対象外 pass の warn は毎フレーム出さず種別ごとに 1 回。
    bool warned_shadow_       = false;
    bool warned_depth_only_   = false;
    bool warned_not_recorded_ = false;
};

#endif // PICTOR_HAS_VULKAN

} // namespace pictor
