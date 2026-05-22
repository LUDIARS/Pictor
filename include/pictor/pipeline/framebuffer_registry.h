#pragma once

/// FramebufferRegistry — `VkFramebuffer` arrays keyed by `(pass_index, slot)`。
///
/// `spec/pipeline-system-b-config.md` §3.4。 swapchain attachment を含む
/// render pass は per-swapchain-image (枚数 = 3 想定)、 それ以外は
/// per-flight (枚数 = 2~3) の framebuffer を 1 本ずつ持つ。
/// init 時に attachment 名 → AttachmentRegistry index に解決して
/// VkFramebuffer を一括生成、 hot path では `get(pass_index, slot)` の
/// flat 配列引きのみ。

// pipeline_profile.h precedes <vulkan/vulkan.h> on purpose — see
// `render_pass_registry.h` for the Win32 `OPAQUE` macro guard rationale.
#include "pictor/pipeline/pipeline_profile.h"

#include <cstdint>
#include <vector>

#ifdef PICTOR_HAS_VULKAN
#ifndef NOGDI
#define NOGDI    // see attachment_registry.h for rationale
#endif
#include <vulkan/vulkan.h>
#endif

namespace pictor {

class AttachmentRegistry;
class RenderPassRegistry;

class FramebufferRegistry {
public:
    FramebufferRegistry();
    ~FramebufferRegistry();

    FramebufferRegistry(const FramebufferRegistry&)            = delete;
    FramebufferRegistry& operator=(const FramebufferRegistry&) = delete;

#ifdef PICTOR_HAS_VULKAN
    /// Build VkFramebuffer arrays for every render pass.
    /// - render passes that contain a SWAPCHAIN_COLOR attachment get
    ///   `swapchain_image_count` framebuffers (per-image)
    /// - all other render passes get `flight_count` framebuffers (per-flight)
    bool initialize_vulkan(VkDevice                   device,
                           const AttachmentRegistry&  attachments,
                           const RenderPassRegistry&  render_passes,
                           uint32_t                   flight_count,
                           uint32_t                   swapchain_image_count);

    /// Recreate framebuffers (e.g. after swapchain resize). Reuses the
    /// previously registered passes / attachments.
    bool resize(uint32_t flight_count, uint32_t swapchain_image_count);

    void shutdown_vulkan();

    /// Hot-path query — per-pass per-slot framebuffer. `slot` is the
    /// flight index for per-flight passes, or the swapchain image index
    /// for per-image passes.
    VkFramebuffer get(uint16_t pass_index, uint32_t slot) const;

    /// True iff `pass_index` is per-swapchain-image (rather than per-flight).
    bool is_swapchain_pass(uint16_t pass_index) const;
#endif

private:
#ifdef PICTOR_HAS_VULKAN
    struct PassEntry {
        std::vector<VkFramebuffer> framebuffers; // flight_count or swapchain_image_count
        bool                       is_swapchain = false;
        VkExtent2D                 extent = {0, 0};
    };

    bool build_one_(uint16_t pass_index,
                    const AttachmentRegistry& attachments,
                    const RenderPassRegistry& render_passes);

    VkDevice                  device_   = VK_NULL_HANDLE;
    uint32_t                  flight_   = 0;
    uint32_t                  swap_     = 0;
    const AttachmentRegistry* attachments_ = nullptr;
    const RenderPassRegistry* render_passes_ = nullptr;
    std::vector<PassEntry>    entries_;
#endif
};

} // namespace pictor
