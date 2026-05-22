#pragma once

/// RenderPassRegistry — VkRenderPass objects keyed by `uint16_t` index.
///
/// `spec/pipeline-system-b-config.md` §3.4。 init 時に
/// `PipelineProfileDef::render_passes[]` を畳み込み、 attachment_ops が
/// 指す attachment 名を `AttachmentRegistry::index_of()` で uint16_t に
/// 解決して VkAttachmentDescription / VkSubpassDescription /
/// VkSubpassDependency を組み立てる。 hot path では index で
/// `VkRenderPass get(uint16_t)` を引くだけ。

// `pipeline_profile.h` must precede `<vulkan/vulkan.h>` so its enum
// initializers (e.g. `PassType::OPAQUE`) are parsed before any Win32
// `OPAQUE` macro pollution. See `postprocess_pipeline.h` for the same
// guard pattern.
#include "pictor/pipeline/attachment_def.h"
#include "pictor/pipeline/pipeline_profile.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#ifdef PICTOR_HAS_VULKAN
#ifndef NOGDI
#define NOGDI    // see attachment_registry.h for rationale
#endif
#include <vulkan/vulkan.h>
#endif

namespace pictor {

class AttachmentRegistry;

class RenderPassRegistry {
public:
    static constexpr uint16_t INVALID_INDEX = 0xFFFFu;

    RenderPassRegistry();
    ~RenderPassRegistry();

    RenderPassRegistry(const RenderPassRegistry&)            = delete;
    RenderPassRegistry& operator=(const RenderPassRegistry&) = delete;

    // ---- Name → index (Vulkan 非依存) -----------------

    /// Register the pass list (does not touch GPU yet). Each pass's
    /// `pass_name` must be unique within the registry.
    void set_passes(std::span<const RenderPassDef> passes);

    uint16_t index_of(std::string_view pass_name) const;
    uint16_t count() const { return static_cast<uint16_t>(passes_.size()); }
    const RenderPassDef& pass(uint16_t i) const { return passes_[i]; }
    std::span<const RenderPassDef> passes() const { return passes_; }

#ifdef PICTOR_HAS_VULKAN
    /// Build VkRenderPass objects for each registered pass.
    /// Attachments referenced in `pass.render_targets[]` are resolved via
    /// `attachments.index_of()`. Missing names → fatal log + skip.
    bool initialize_vulkan(VkDevice device, const AttachmentRegistry& attachments);

    void shutdown_vulkan();

    VkRenderPass get(uint16_t index) const {
        return (index < passes_render_passes_.size())
            ? passes_render_passes_[index]
            : VK_NULL_HANDLE;
    }
#endif

private:
    std::vector<RenderPassDef> passes_;

#ifdef PICTOR_HAS_VULKAN
    bool build_one_(uint16_t index, const AttachmentRegistry& attachments);

    VkDevice                  device_ = VK_NULL_HANDLE;
    std::vector<VkRenderPass> passes_render_passes_;
#endif
};

} // namespace pictor
