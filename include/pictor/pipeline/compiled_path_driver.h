#pragma once

/// CompiledPathDriver — profile → CompiledGraph のライフサイクル管理。
///
/// `spec/pipeline-system-b-config.md` §3.5 / review/2026-06-11 D-2 の配線層。
/// `PipelineCompiler::compile()` は「起動時 / プロファイル切替 / swapchain
/// resize の 1 回だけ」呼ぶ規約だが、 その再 compile 判断と、 旧 graph の
/// リソース解放 (descriptor pool) を伴う差し替え手順は compile 関数自身の
/// 責務ではない。 本クラスがその 1 責務 (SRP) を持つ:
///
///   engage()    — registries / device を借用して初回 compile + scheduler へ install
///   recompile() — プロファイル切替 / swapchain resize 時のみ呼ぶ (毎フレーム不可)
///   disengage() — graph を回収して descriptor pool を解放 (device 破棄前に必須)
///
/// 所有権: registries / device は借用 (host 所有)。 CompiledGraph は
/// scheduler に move で預けるが、 解放責任は本クラスが負う (全経路で解放)。

// pipeline_profile.h precedes <vulkan/vulkan.h> — see render_pass_registry.h.
#include "pictor/pipeline/compiled_graph.h"
#include "pictor/pipeline/pipeline_profile.h"

#include <cstdint>

namespace pictor {

class AttachmentRegistry;
class RenderPassRegistry;
class FramebufferRegistry;
class RenderPassScheduler;

#ifdef PICTOR_HAS_VULKAN

class CompiledPathDriver {
public:
    /// Compile に要る借用依存の束。 registries は host が構築済みであること。
    /// `device == VK_NULL_HANDLE` は headless (テスト / 構造検証) モード:
    /// descriptor pool を作らず、 VkHandle は全て VK_NULL_HANDLE のまま
    /// graph 構造だけを組む (PipelineCompiler 側の同名ガードに対応)。
    struct Deps {
        VkDevice                   device                = VK_NULL_HANDLE;
        const AttachmentRegistry*  attachments           = nullptr;
        const RenderPassRegistry*  render_passes         = nullptr;
        const FramebufferRegistry* framebuffers          = nullptr;
        uint32_t                   flight_count          = 2;
        uint32_t                   swapchain_image_count = 3;
    };

    CompiledPathDriver() = default;
    ~CompiledPathDriver() = default;

    CompiledPathDriver(const CompiledPathDriver&)            = delete;
    CompiledPathDriver& operator=(const CompiledPathDriver&) = delete;

    /// 初回 compile + scheduler への install。 依存を保持し engaged 状態にする。
    /// registry が欠けている場合は false (§7.1: 設定不備は無言で通さない)。
    bool engage(const Deps& deps,
                const PipelineProfileDef& profile,
                RenderPassScheduler& scheduler);

    /// プロファイル切替 / swapchain resize 時の再 compile。
    /// 旧 graph を scheduler から回収して解放してから差し替える。
    /// engage() 前に呼ばれたら false。
    bool recompile(const PipelineProfileDef& profile,
                   RenderPassScheduler& scheduler);

    /// graph を回収して descriptor pool を解放し、 disengaged に戻す。
    /// VkDevice 破棄前に必ず呼ぶこと (リソースは全経路で解放)。
    void disengage(RenderPassScheduler& scheduler);

    bool engaged() const { return engaged_; }

private:
    /// 旧 graph 回収 → compile → install の共通手順。
    bool compile_and_install_(const PipelineProfileDef& profile,
                              RenderPassScheduler& scheduler);

    /// 旧 graph の解放 (非空なら GPU idle 待ちの上で descriptor pool を破棄)。
    void release_old_graph_(CompiledGraph& old);

    Deps deps_{};
    bool engaged_ = false;
};

#else // !PICTOR_HAS_VULKAN

/// Headless build: API 形状だけ保つ (graph は常に空)。
class CompiledPathDriver {
public:
    struct Deps {};
    bool engage(const Deps&, const PipelineProfileDef&, RenderPassScheduler&) { return false; }
    bool recompile(const PipelineProfileDef&, RenderPassScheduler&) { return false; }
    void disengage(RenderPassScheduler&) {}
    bool engaged() const { return false; }
};

#endif // PICTOR_HAS_VULKAN

} // namespace pictor
