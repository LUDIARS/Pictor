#include "pictor/pipeline/pipeline_compiler.h"
#include "pictor/pipeline/attachment_registry.h"
#include "pictor/pipeline/render_pass_registry.h"
#include "pictor/pipeline/framebuffer_registry.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace pictor {

#ifdef PICTOR_HAS_VULKAN

// ============================================================
// CompiledGraph teardown
// ============================================================

void CompiledGraph::shutdown(VkDevice device) {
    if (descriptor_pool != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
    }
    descriptor_pool = VK_NULL_HANDLE;
    descriptor_sets_storage.clear();
    passes.clear();
    name_storage.clear();
}

// ============================================================
// PipelineCompiler
// ============================================================

namespace {

// Pack a copy of `name` into `storage` (appending a null terminator) and
// return a stable pointer into storage. `storage` must not be re-allocated
// after this — caller should reserve() up front.
const char* pack_name(std::vector<char>& storage, const std::string& name) {
    const size_t off = storage.size();
    storage.insert(storage.end(), name.begin(), name.end());
    storage.push_back('\0');
    return storage.data() + off;
}

bool pass_has_swapchain_target(const RenderPassDef& pd, const AttachmentRegistry& atts) {
    for (const std::string& t : pd.render_targets) {
        uint16_t idx = atts.index_of(t);
        if (idx == AttachmentRegistry::INVALID_INDEX) continue;
        if (atts.def(idx).kind == AttachmentKind::SWAPCHAIN_COLOR) return true;
    }
    return false;
}

void fill_clear_values(const RenderPassDef& pd,
                       const AttachmentRegistry& atts,
                       CompiledPass& cp)
{
    cp.clear_count = 0;
    for (const std::string& t : pd.render_targets) {
        if (cp.clear_count >= cp.clear_values.size()) break;
        uint16_t idx = atts.index_of(t);
        if (idx == AttachmentRegistry::INVALID_INDEX) {
            // Unknown attachment — write zeroes to keep VkRenderPassBeginInfo valid.
            cp.clear_values[cp.clear_count] = VkClearValue{};
            cp.clear_count++;
            continue;
        }
        const AttachmentDef& d = atts.def(idx);
        VkClearValue cv{};
        if (d.kind == AttachmentKind::DEPTH) {
            cv.depthStencil.depth   = d.clear_depth;
            cv.depthStencil.stencil = d.clear_stencil;
        } else {
            cv.color.float32[0] = d.clear_color[0];
            cv.color.float32[1] = d.clear_color[1];
            cv.color.float32[2] = d.clear_color[2];
            cv.color.float32[3] = d.clear_color[3];
        }
        cp.clear_values[cp.clear_count] = cv;
        cp.clear_count++;
    }
}

VkExtent2D resolve_pass_extent(const RenderPassDef& pd,
                                const AttachmentRegistry& atts)
{
    VkExtent2D e{0, 0};
    for (const std::string& t : pd.render_targets) {
        uint16_t idx = atts.index_of(t);
        if (idx == AttachmentRegistry::INVALID_INDEX) continue;
        VkExtent2D ae = atts.extent(idx);
        if (e.width  == 0) e.width  = ae.width;
        if (e.height == 0) e.height = ae.height;
    }
    return e;
}

} // anon

CompiledGraph PipelineCompiler::compile(const PipelineProfileDef&  profile,
                                         const AttachmentRegistry&  atts,
                                         const RenderPassRegistry&  rps,
                                         const FramebufferRegistry& fbs,
                                         VkDevice                   device,
                                         uint32_t                   flight_count,
                                         uint32_t                   swapchain_image_count)
{
    CompiledGraph g;
    if (profile.render_passes.empty()) {
        return g;
    }

    // Reserve a chunky name buffer (avoids reallocation moving char* views).
    size_t name_budget = 0;
    for (const auto& pd : profile.render_passes) name_budget += pd.pass_name.size() + 1;
    g.name_storage.reserve(name_budget);
    g.passes.reserve(profile.render_passes.size());

    // ---- Allocate descriptor sets up-front --------------------------------
    // input_textures binding layout is currently fixed (sampled image
    // 0..N-1). Phase 4 will replace this with per-pass layouts. For now we
    // size the pool generously for `passes * flight_count` sets across the
    // worst-case binding count (8).
    const uint32_t max_sets        = std::max(1u, static_cast<uint32_t>(
            profile.render_passes.size()) * std::max(1u, flight_count));
    const uint32_t max_descriptors = max_sets * 8u; // 8 sampled images per set

    VkDescriptorPoolSize pool_size{};
    pool_size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = max_descriptors;

    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets       = max_sets;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes    = &pool_size;

    // device == VK_NULL_HANDLE は headless compile (unit test / 構造検証)。
    // vkCreateDescriptorPool を null device に投げると loader が落ちるため
    // pool を作らず進む — passes の構造 (order / flags / clear / debug_name)
    // は GPU 無しでも決定的に組める。 input_sets は全 NULL のまま。
    if (device == VK_NULL_HANDLE) {
        g.descriptor_pool = VK_NULL_HANDLE;
    } else if (vkCreateDescriptorPool(device, &dpci, nullptr, &g.descriptor_pool) != VK_SUCCESS) {
        std::fprintf(stderr, "[PipelineCompiler] vkCreateDescriptorPool failed (max_sets=%u)\n",
                     max_sets);
        // Continue without input_sets — passes will still execute, just no
        // bound input textures (host stays responsible for binding fallback).
        g.descriptor_pool = VK_NULL_HANDLE;
    }

    // ---- Per-pass compile -------------------------------------------------
    for (uint16_t i = 0; i < static_cast<uint16_t>(profile.render_passes.size()); ++i) {
        const RenderPassDef& pd = profile.render_passes[i];

        CompiledPass cp{};
        cp.debug_name  = pack_name(g.name_storage, pd.pass_name);
        cp.filter_mask = pd.filter_mask;
        cp.sort_mode   = static_cast<uint8_t>(pd.sort_mode);
        cp.pass_type   = static_cast<uint8_t>(pd.pass_type);

        // Look up the VkRenderPass via the pass name (only place we use a
        // string — init time, not hot path).
        cp.pass_id = rps.index_of(pd.pass_name);
        cp.render_pass = rps.get(cp.pass_id);
        if (cp.render_pass == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[PipelineCompiler] pass '%s' has no VkRenderPass (registry built?)\n",
                         pd.pass_name.c_str());
        }

        // Determine slot type (per-flight vs per-image) and pull framebuffers.
        const bool is_swap = pass_has_swapchain_target(pd, atts);
        if (is_swap) cp.flags |= CompiledPass::FLAG_IS_SWAPCHAIN;
        if (pd.pass_type == PassType::COMPUTE) cp.flags |= CompiledPass::FLAG_IS_COMPUTE;
        // host_recorded フラグは pass def から伝播。 execute_compiled は
        // Begin/End render pass / pipeline bind を一切発行せず host record に
        // 完全委譲する (Phase 4 step 3 / PostProcessPipeline 等の chain pass)。
        if (pd.host_recorded) cp.flags |= CompiledPass::FLAG_IS_HOST_RECORDED;

        const uint32_t slot_count = is_swap ? swapchain_image_count : flight_count;
        const uint32_t copy_count = std::min<uint32_t>(slot_count, static_cast<uint32_t>(cp.framebuffers.size()));
        cp.framebuffer_count = static_cast<uint8_t>(copy_count);
        for (uint32_t s = 0; s < copy_count; ++s) {
            cp.framebuffers[s] = fbs.get(i, s);
        }

        // ---- input_sets — allocate one set per flight (current layout) ----
        // Layout: sampled image array of size `min(8, input_textures.size())`.
        // For now we allocate but do not populate writes (host wires actual
        // bindings on first use). This stays correct because hot-path
        // `vkCmdBindDescriptorSets` is a no-op for empty layouts.
        // Phase 4 will replace the layout with per-pass typed layouts.
        // -----------------------------------------------------------------
        for (uint32_t f = 0; f < std::min<uint32_t>(flight_count, cp.input_sets.size()); ++f) {
            cp.input_sets[f] = VK_NULL_HANDLE; // populated by host wiring (Phase 4)
        }

        // Render area + clears.
        VkExtent2D ext = resolve_pass_extent(pd, atts);
        cp.render_area.offset = {0, 0};
        cp.render_area.extent = ext;
        fill_clear_values(pd, atts, cp);

        // pipeline override: ShaderRegistry resolution is host-driven for
        // now (Pictor's ShaderRegistry::pipeline(handle) returns VkPipeline).
        // PipelineCompiler doesn't depend on ShaderRegistry yet — host code
        // sets `cp.pipeline` post-compile when wiring custom passes. Keep
        // NULL here for the v1 path.
        cp.pipeline = VK_NULL_HANDLE;

        g.passes.push_back(cp);
    }

    return g;
}

#endif // PICTOR_HAS_VULKAN

} // namespace pictor
