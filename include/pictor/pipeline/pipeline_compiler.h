#pragma once

/// PipelineCompiler — `PipelineProfileDef` → `CompiledGraph` の畳み込み。
///
/// \`spec/pipeline-system-b-config.md\` §3.5。 起動時 / プロファイル切替 /
/// swapchain resize の 1 回だけ呼ぶ。 全 string 名を VkHandle / uint16_t
/// に解決し、 \`RenderPassScheduler\` の hot path から \`std::string\` を消す。

// pipeline_profile.h precedes <vulkan/vulkan.h> — see render_pass_registry.h.
#include "pictor/pipeline/compiled_graph.h"
#include "pictor/pipeline/pipeline_profile.h"

#include <cstdint>

#ifdef PICTOR_HAS_VULKAN
#ifndef NOGDI
#define NOGDI    // see attachment_registry.h
#endif
#include <vulkan/vulkan.h>
#endif

namespace pictor {

class AttachmentRegistry;
class RenderPassRegistry;
class FramebufferRegistry;

#ifdef PICTOR_HAS_VULKAN

class PipelineCompiler {
public:
    /// Build a CompiledGraph from a profile + registries. Returns an
    /// empty graph (passes_.empty()) on failure (with stderr diagnostics).
    ///
    /// Ownership: the returned CompiledGraph owns its VkDescriptorPool /
    /// VkDescriptorSets. Caller must call `graph.shutdown(device)` before
    /// device destruction (or before reassigning over a live graph).
    /// VkRenderPass / VkFramebuffer / VkImageView pointers are owned by
    /// the registries — the graph only borrows them.
    static CompiledGraph compile(const PipelineProfileDef&  profile,
                                  const AttachmentRegistry&  attachments,
                                  const RenderPassRegistry&  render_passes,
                                  const FramebufferRegistry& framebuffers,
                                  VkDevice                   device,
                                  uint32_t                   flight_count,
                                  uint32_t                   swapchain_image_count);
};

#else // !PICTOR_HAS_VULKAN

class PipelineCompiler {
public:
    /// Headless build: returns an empty graph (no GPU work).
    static CompiledGraph compile(const PipelineProfileDef&) {
        return CompiledGraph{};
    }
};

#endif // PICTOR_HAS_VULKAN

} // namespace pictor
