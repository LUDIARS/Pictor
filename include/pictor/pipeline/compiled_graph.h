#pragma once

/// CompiledGraph — hot-path 用 flat 表現 (Phase 3 §3.5)。
///
/// `PipelineCompiler::compile()` が `PipelineProfileDef::render_passes[]`
/// (宣言形式の string 参照だらけ) を畳み込み、 全 string を
/// VkRenderPass / VkFramebuffer / VkPipeline / uint16_t に解決した
/// `std::vector<CompiledPass>` を作る。 `RenderPassScheduler` は
/// `execute_compiled()` でこの配列を seq-iterate するだけ。
///
/// 不変条件 (\`spec/pipeline-system-b-config.md\` §3.5):
///   - hot path に \`std::string\` / \`std::unordered_map\` ゼロ
///   - per-pass の VkHandle / index は直値 (init 1 回で解決済)
///   - debug_name は \`const char*\` の static-lived literal のみ、 hot path で読まない

// pipeline_profile.h precedes <vulkan/vulkan.h> — see render_pass_registry.h.
#include "pictor/pipeline/pipeline_profile.h"

#include <array>
#include <cstdint>
#include <vector>

#ifdef PICTOR_HAS_VULKAN
#ifndef NOGDI
#define NOGDI    // see attachment_registry.h
#endif
#include <vulkan/vulkan.h>
#endif

namespace pictor {

#ifdef PICTOR_HAS_VULKAN

struct CompiledPass {
    // ---- frame loop direct values ---------------------------------------
    /// VkRenderPass (resolved once by PipelineCompiler — never null after compile)
    VkRenderPass    render_pass = VK_NULL_HANDLE;

    /// Up to 4 framebuffers. `framebuffer_count` indexes how many are valid;
    /// `is_swapchain` selects whether `slot` is the swapchain image index or
    /// the flight index (single branch in hot path).
    std::array<VkFramebuffer, 4>   framebuffers{};
    std::array<VkDescriptorSet, 4> input_sets{};   // 0..flight_count - pre-built

    // ---- render area + clear values (RenderPassBeginInfo に直接渡せる) ----
    VkRect2D                       render_area{};
    uint16_t                       clear_count = 0;
    std::array<VkClearValue, 8>    clear_values{};

    // ---- pipeline override (NULL = host が pass_type ごとの既定 pipeline を bind) --
    VkPipeline                     pipeline = VK_NULL_HANDLE;

    // ---- batch 抽出ヒント (uint で済む、 hot path で OK) -----------------
    uint32_t                       filter_mask = 0xFFFFFFFFu;
    uint8_t                        sort_mode   = 0;   // SortMode enum を 1 byte で
    uint8_t                        pass_type   = 0;   // PassType enum を 1 byte で
    uint8_t                        framebuffer_count = 1;
    uint8_t                        flags       = 0;   // bit0 = is_swapchain, bit1 = is_compute

    // ---- 観測用 (Phase 2 §6.1 GPU timestamp タグ) ------------------------
    uint16_t                       timestamp_begin_query = 0xFFFFu;
    uint16_t                       timestamp_end_query   = 0xFFFFu;

    // ---- debug ----------------------------------------------------------
    /// Static-lived literal (\`pass.pass_name\` から strdup ではなく、
    /// CompiledGraph::name_storage_ が所有する null-terminated コピーへの
    /// ポインタ)。 hot path では読まない。
    const char*                    debug_name = nullptr;

    static constexpr uint8_t FLAG_IS_SWAPCHAIN    = 1u << 0;
    static constexpr uint8_t FLAG_IS_COMPUTE      = 1u << 1;
    /// host_recorded: execute_compiled は Begin/End render pass / pipeline bind を
    /// 一切発行せず、 PassRecordFn 内で host が完全に command 記録を行う。
    /// PostProcessPipeline のように内部で複数 sub-render-pass を持つ chain を
    /// 単一 CompiledGraph entry として表現するための逃げ口。 KS の
    /// `kuzu.profile.json` PostProcess pass で使う。 compute pass と類似
    /// (Begin/End 不要)、 ただし graphics queue で動く点が違うので別 flag。
    static constexpr uint8_t FLAG_IS_HOST_RECORDED = 1u << 2;

    bool is_swapchain()     const { return (flags & FLAG_IS_SWAPCHAIN) != 0; }
    bool is_compute()       const { return (flags & FLAG_IS_COMPUTE) != 0; }
    bool is_host_recorded() const { return (flags & FLAG_IS_HOST_RECORDED) != 0; }
};
static_assert(sizeof(CompiledPass) <= 256,
              "CompiledPass should stay cache-friendly (≤256B target)");

struct CompiledGraph {
    std::vector<CompiledPass>   passes;

    // Backing storage (owned, lifetime ≥ passes_).
    VkDescriptorPool            descriptor_pool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptor_sets_storage;
    std::vector<char>           name_storage;   // packed null-terminated names

    /// Destroy the descriptor pool / sets and clear all state.
    /// Caller must call this before letting the graph go out of scope while
    /// the device is still alive.
    void shutdown(VkDevice device);
};

#else  // !PICTOR_HAS_VULKAN

// Headless build: keep the type declared so callers compile, but stub it.
struct CompiledPass {
    uint8_t  pass_type = 0;
    uint8_t  flags     = 0;
    uint32_t filter_mask = 0;
    const char* debug_name = nullptr;
};
struct CompiledGraph {
    std::vector<CompiledPass> passes;
};

#endif // PICTOR_HAS_VULKAN

} // namespace pictor
