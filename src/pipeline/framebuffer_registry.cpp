#include "pictor/pipeline/framebuffer_registry.h"
#include "pictor/pipeline/attachment_registry.h"
#include "pictor/pipeline/render_pass_registry.h"

#include <cstdio>

namespace pictor {

FramebufferRegistry::FramebufferRegistry() = default;

FramebufferRegistry::~FramebufferRegistry() {
#ifdef PICTOR_HAS_VULKAN
    shutdown_vulkan();
#endif
}

#ifdef PICTOR_HAS_VULKAN

bool FramebufferRegistry::build_one_(uint16_t pass_index,
                                      const AttachmentRegistry& atts,
                                      const RenderPassRegistry& rps)
{
    const RenderPassDef& pd = rps.pass(pass_index);
    PassEntry& e = entries_[pass_index];

    // Host-managed multi-pass chains: host (application) が内部で
    // Begin/End / Pipeline bind を自前管理するため、 framework 側で
    // framebuffer を作る必要は無い。 RenderPassRegistry も同様に
    // VkRenderPass を生成しない (空 handle のまま) ので、 ここで build を
    // 試みると「has no VkRenderPass」 で必ず失敗する。
    // 内部で複数 sub-render-pass を持つチェーンを CompiledGraph entry として
    // 1 つで表現するための逃げ口 (Pictor PR #66 で導入)。
    if (pd.host_recorded) {
        e.is_swapchain = false;
        e.extent       = {0, 0};
        e.framebuffers.clear();
        return true;
    }

    // Look up attachment indices once.
    std::vector<uint16_t> att_indices;
    att_indices.reserve(pd.render_targets.size());
    bool has_swapchain = false;
    VkExtent2D pass_extent = {0, 0};

    for (const std::string& tgt : pd.render_targets) {
        uint16_t idx = atts.index_of(tgt);
        if (idx == AttachmentRegistry::INVALID_INDEX) {
            std::fprintf(stderr, "[FramebufferRegistry] pass '%s' refs unknown attachment '%s'\n",
                         pd.pass_name.c_str(), tgt.c_str());
            return false;
        }
        att_indices.push_back(idx);

        const AttachmentDef& d = atts.def(idx);
        if (d.kind == AttachmentKind::SWAPCHAIN_COLOR) has_swapchain = true;

        VkExtent2D e2 = atts.extent(idx);
        if (pass_extent.width == 0)  pass_extent.width  = e2.width;
        if (pass_extent.height == 0) pass_extent.height = e2.height;
    }

    e.is_swapchain = has_swapchain;
    e.extent       = pass_extent;
    const uint32_t slots = has_swapchain ? swap_ : flight_;
    e.framebuffers.assign(slots, VK_NULL_HANDLE);

    VkRenderPass rp = rps.get(pass_index);
    if (rp == VK_NULL_HANDLE) {
        std::fprintf(stderr, "[FramebufferRegistry] pass '%s' has no VkRenderPass\n",
                     pd.pass_name.c_str());
        return false;
    }

    std::vector<VkImageView> views(att_indices.size(), VK_NULL_HANDLE);

    for (uint32_t slot = 0; slot < slots; ++slot) {
        for (size_t i = 0; i < att_indices.size(); ++i) {
            const AttachmentDef& d = atts.def(att_indices[i]);
            // Per-swapchain-image attachment indexes by `slot` (swap image idx).
            // Per-flight attachment indexes by `slot` when whole pass is
            // per-flight; per-swapchain pass uses slot for swapchain attachment
            // but a single per-flight resource for non-swapchain attachments —
            // for simplicity, use slot for everything and rely on min-slot wrap.
            uint32_t att_slot = slot;
            if (d.kind != AttachmentKind::SWAPCHAIN_COLOR) {
                // wrap into the attachment's slot range (flight_count)
                uint32_t avail = atts.slot_count(att_indices[i]);
                if (avail == 0) {
                    std::fprintf(stderr, "[FramebufferRegistry] att '%s' has 0 slots\n",
                                 d.name.c_str());
                    return false;
                }
                att_slot = slot % avail;
            }
            views[i] = atts.view(att_indices[i], att_slot);
            if (views[i] == VK_NULL_HANDLE) {
                std::fprintf(stderr, "[FramebufferRegistry] missing view for '%s' slot %u\n",
                             d.name.c_str(), att_slot);
                return false;
            }
        }

        VkFramebufferCreateInfo fb{};
        fb.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass      = rp;
        fb.attachmentCount = static_cast<uint32_t>(views.size());
        fb.pAttachments    = views.data();
        fb.width           = pass_extent.width  ? pass_extent.width  : 1;
        fb.height          = pass_extent.height ? pass_extent.height : 1;
        fb.layers          = 1;

        if (vkCreateFramebuffer(device_, &fb, nullptr, &e.framebuffers[slot]) != VK_SUCCESS) {
            std::fprintf(stderr, "[FramebufferRegistry] vkCreateFramebuffer failed (pass '%s' slot %u)\n",
                         pd.pass_name.c_str(), slot);
            return false;
        }
    }
    return true;
}

bool FramebufferRegistry::initialize_vulkan(VkDevice                   device,
                                             const AttachmentRegistry&  attachments,
                                             const RenderPassRegistry&  render_passes,
                                             uint32_t                   flight_count,
                                             uint32_t                   swapchain_image_count)
{
    device_         = device;
    flight_         = flight_count;
    swap_           = swapchain_image_count;
    attachments_    = &attachments;
    render_passes_  = &render_passes;
    entries_.assign(render_passes.count(), {});

    for (uint16_t i = 0; i < render_passes.count(); ++i) {
        if (!build_one_(i, attachments, render_passes)) return false;
    }
    return true;
}

bool FramebufferRegistry::resize(uint32_t flight_count, uint32_t swapchain_image_count) {
    if (!device_ || !attachments_ || !render_passes_) return false;

    // Destroy all framebuffers.
    for (auto& e : entries_) {
        for (VkFramebuffer fb : e.framebuffers) {
            if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
        }
        e.framebuffers.clear();
    }

    flight_ = flight_count;
    swap_   = swapchain_image_count;

    for (uint16_t i = 0; i < render_passes_->count(); ++i) {
        if (!build_one_(i, *attachments_, *render_passes_)) return false;
    }
    return true;
}

void FramebufferRegistry::shutdown_vulkan() {
    if (!device_) {
        entries_.clear();
        return;
    }
    for (auto& e : entries_) {
        for (VkFramebuffer fb : e.framebuffers) {
            if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
        }
        e.framebuffers.clear();
    }
    entries_.clear();
    device_         = VK_NULL_HANDLE;
    attachments_    = nullptr;
    render_passes_  = nullptr;
}

VkFramebuffer FramebufferRegistry::get(uint16_t pass_index, uint32_t slot) const {
    if (pass_index >= entries_.size()) return VK_NULL_HANDLE;
    const PassEntry& e = entries_[pass_index];
    if (slot >= e.framebuffers.size()) return VK_NULL_HANDLE;
    return e.framebuffers[slot];
}

bool FramebufferRegistry::is_swapchain_pass(uint16_t pass_index) const {
    if (pass_index >= entries_.size()) return false;
    return entries_[pass_index].is_swapchain;
}

#endif // PICTOR_HAS_VULKAN

} // namespace pictor
