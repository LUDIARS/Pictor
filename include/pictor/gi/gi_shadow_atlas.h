#pragma once

/// GIShadowAtlas — CSM 影アトラスの host-driven 描画契約 (phase 2)。
///
/// Pictor はメッシュを所有しないため、 影の depth 描画はホスト責務
/// (`PostProcessPipeline` の scene render pass と同型の契約 —
/// `spec/feature/gi-bake-realtime-design.md` §2.5)。 本クラスは
/// depth array image (cascade ごとに 1 layer) / render pass /
/// per-cascade framebuffer / compare sampler を所有する。
///
/// ホストの手順 (毎フレーム):
///   1. `GILightingSystem::execute()` で cascade 行列を更新
///      (`cascade_view_proj()` / `shadow_uniforms()`)。
///   2. cascade ごとに `render_pass()` + `framebuffer(c)` で depth pass を
///      begin し、 影を落とすメッシュを light view-proj で描く
///      (`shaders/shadow_depth.vert` / `.frag` 提供済み — pipeline は
///      ホストの頂点レイアウトで組む)。
///   3. マテリアル側 (pbr.frag の shadow.glsl) へ `atlas_view()` +
///      `compare_sampler()` と `ShadowUniformData` を結線する。
///
/// SRP: アトラス資源の所有のみ。 cascade 分割 / 行列は `GILightingSystem`。

#include "pictor/core/types.h"

#ifdef PICTOR_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace pictor {

class VulkanContext;

class GIShadowAtlas {
public:
    static constexpr uint32_t kMaxCascades = 4;

    GIShadowAtlas() = default;
    ~GIShadowAtlas();

    GIShadowAtlas(const GIShadowAtlas&) = delete;
    GIShadowAtlas& operator=(const GIShadowAtlas&) = delete;

    bool is_initialized() const { return initialized_; }

#ifdef PICTOR_HAS_VULKAN
    /// depth array image (resolution² × cascade_count layers、 D32_SFLOAT) と
    /// render pass / framebuffer / sampler を確保する。
    bool initialize(VulkanContext& vk, uint32_t resolution,
                    uint32_t cascade_count);

    /// depth-only render pass (CLEAR → finalLayout SHADER_READ_ONLY)。
    /// ホストは clearValueCount=1 (depthStencil = {1.0, 0}) で begin する。
    VkRenderPass render_pass() const { return render_pass_; }

    /// cascade ごとの framebuffer (対応する array layer に描く)。
    VkFramebuffer framebuffer(uint32_t cascade) const {
        return cascade < cascade_count_ ? framebuffers_[cascade]
                                        : VK_NULL_HANDLE;
    }

    /// 全 cascade を束ねた array view (shadow.glsl の sampler2DArray 用)。
    VkImageView atlas_view() const { return atlas_view_; }

    /// PCF 用 compare sampler (LESS_OR_EQUAL、 CLAMP_TO_BORDER 白)。
    VkSampler compare_sampler() const { return compare_sampler_; }
#endif

    uint32_t resolution() const    { return resolution_; }
    uint32_t cascade_count() const { return cascade_count_; }

    void shutdown();

private:
    uint32_t resolution_    = 0;
    uint32_t cascade_count_ = 0;
    bool     initialized_   = false;

#ifdef PICTOR_HAS_VULKAN
    VulkanContext* vk_     = nullptr;
    VkDevice       device_ = VK_NULL_HANDLE;

    VkImage        atlas_image_  = VK_NULL_HANDLE;
    VkDeviceMemory atlas_memory_ = VK_NULL_HANDLE;
    VkImageView    atlas_view_   = VK_NULL_HANDLE;            // array 全層
    VkImageView    layer_views_[kMaxCascades] = {};           // framebuffer 用
    VkFramebuffer  framebuffers_[kMaxCascades] = {};
    VkRenderPass   render_pass_    = VK_NULL_HANDLE;
    VkSampler      compare_sampler_ = VK_NULL_HANDLE;
#endif
};

} // namespace pictor
