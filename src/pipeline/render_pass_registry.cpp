#include "pictor/pipeline/render_pass_registry.h"
#include "pictor/pipeline/attachment_registry.h"

#include <cstdio>

namespace pictor {

RenderPassRegistry::RenderPassRegistry() = default;

RenderPassRegistry::~RenderPassRegistry() {
#ifdef PICTOR_HAS_VULKAN
    shutdown_vulkan();
#endif
}

void RenderPassRegistry::set_passes(std::span<const RenderPassDef> passes) {
    passes_.assign(passes.begin(), passes.end());
#ifdef PICTOR_HAS_VULKAN
    if (!passes_render_passes_.empty()) {
        shutdown_vulkan();
    }
#endif
}

uint16_t RenderPassRegistry::index_of(std::string_view pass_name) const {
    for (size_t i = 0; i < passes_.size(); ++i) {
        if (passes_[i].pass_name == pass_name) return static_cast<uint16_t>(i);
    }
    return INVALID_INDEX;
}

#ifdef PICTOR_HAS_VULKAN

namespace {

// Default ops if `RenderPassDef::attachment_ops` is empty.
AttachmentOpsDef infer_op(const std::string& target, const AttachmentRegistry& atts) {
    uint16_t idx = atts.index_of(target);
    AttachmentOpsDef op;
    op.attachment_name = target;
    if (idx == AttachmentRegistry::INVALID_INDEX) {
        // Unknown — keep CLEAR/STORE/UNDEFINED→SHADER_READ_ONLY.
        return op;
    }
    const AttachmentDef& d = atts.def(idx);
    if (d.kind == AttachmentKind::DEPTH) {
        op.load_op        = AttachmentLoadOp::CLEAR;
        op.store_op       = AttachmentStoreOp::DONT_CARE;
        op.initial_layout = ImageLayout::UNDEFINED;
        op.final_layout   = ImageLayout::DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    } else if (d.kind == AttachmentKind::SWAPCHAIN_COLOR) {
        op.load_op        = AttachmentLoadOp::CLEAR;
        op.store_op       = AttachmentStoreOp::STORE;
        op.initial_layout = ImageLayout::UNDEFINED;
        op.final_layout   = ImageLayout::PRESENT_SRC_KHR;
    }
    return op;
}

} // anon

bool RenderPassRegistry::build_one_(uint16_t index, const AttachmentRegistry& atts) {
    const RenderPassDef& pd = passes_[index];

    // Compute view (attachment_ops, in render_targets order).
    std::vector<AttachmentOpsDef> resolved;
    resolved.reserve(pd.render_targets.size());
    for (size_t t = 0; t < pd.render_targets.size(); ++t) {
        const std::string& tgt = pd.render_targets[t];

        // Look up explicit ops for this attachment, otherwise infer default.
        const AttachmentOpsDef* explicit_op = nullptr;
        for (const auto& o : pd.attachment_ops) {
            if (o.attachment_name == tgt) { explicit_op = &o; break; }
        }
        resolved.push_back(explicit_op ? *explicit_op : infer_op(tgt, atts));
    }

    // VkAttachmentDescription + VkAttachmentReference (single subpass).
    std::vector<VkAttachmentDescription> ads;
    std::vector<VkAttachmentReference>   color_refs;
    VkAttachmentReference                depth_ref{};
    bool has_depth = false;

    ads.reserve(resolved.size());
    color_refs.reserve(resolved.size());

    for (size_t i = 0; i < resolved.size(); ++i) {
        uint16_t att_idx = atts.index_of(resolved[i].attachment_name);
        if (att_idx == AttachmentRegistry::INVALID_INDEX) {
            std::fprintf(stderr, "[RenderPassRegistry] pass '%s' references unknown attachment '%s'\n",
                         pd.pass_name.c_str(), resolved[i].attachment_name.c_str());
            return false;
        }
        const AttachmentDef& d = atts.def(att_idx);

        VkAttachmentDescription ad{};
        ad.format         = atts.vk_format(att_idx);
        ad.samples        = VK_SAMPLE_COUNT_1_BIT;
        ad.loadOp         = to_vk_load_op(resolved[i].load_op);
        ad.storeOp        = to_vk_store_op(resolved[i].store_op);
        ad.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ad.initialLayout  = to_vk_layout(resolved[i].initial_layout);
        ad.finalLayout    = to_vk_layout(resolved[i].final_layout);
        ads.push_back(ad);

        VkAttachmentReference ref{};
        ref.attachment = static_cast<uint32_t>(i);
        if (d.kind == AttachmentKind::DEPTH) {
            ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth_ref  = ref;
            has_depth  = true;
        } else {
            ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color_refs.push_back(ref);
        }
    }

    VkSubpassDescription sub{};
    sub.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount    = static_cast<uint32_t>(color_refs.size());
    sub.pColorAttachments       = color_refs.empty() ? nullptr : color_refs.data();
    sub.pDepthStencilAttachment = has_depth ? &depth_ref : nullptr;

    // Standard external→subpass dependency for present / shader read sequences.
    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci{};
    ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = static_cast<uint32_t>(ads.size());
    ci.pAttachments    = ads.empty() ? nullptr : ads.data();
    ci.subpassCount    = 1;
    ci.pSubpasses      = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies   = &dep;

    if (vkCreateRenderPass(device_, &ci, nullptr, &passes_render_passes_[index]) != VK_SUCCESS) {
        std::fprintf(stderr, "[RenderPassRegistry] vkCreateRenderPass failed for '%s'\n",
                     pd.pass_name.c_str());
        return false;
    }
    return true;
}

bool RenderPassRegistry::initialize_vulkan(VkDevice device, const AttachmentRegistry& atts) {
    device_ = device;
    passes_render_passes_.assign(passes_.size(), VK_NULL_HANDLE);
    for (uint16_t i = 0; i < static_cast<uint16_t>(passes_.size()); ++i) {
        if (!build_one_(i, atts)) return false;
    }
    return true;
}

void RenderPassRegistry::shutdown_vulkan() {
    if (!device_) {
        passes_render_passes_.clear();
        return;
    }
    for (VkRenderPass rp : passes_render_passes_) {
        if (rp) vkDestroyRenderPass(device_, rp, nullptr);
    }
    passes_render_passes_.clear();
    device_ = VK_NULL_HANDLE;
}

#endif // PICTOR_HAS_VULKAN

} // namespace pictor
