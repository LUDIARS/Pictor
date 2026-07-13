#pragma once

/// GIReflectionProbe — reflection probe (cubemap) の host-driven 描画契約
/// (phase 3)。 RGBA16F の mip 付き cubemap / face ごとの render pass +
/// framebuffer / blit による mip 生成 / サンプラを所有する。
/// シーンの 6 面描画はホスト責務 (GIShadowAtlas と同型の契約)。
///
/// ホストの手順 (キャプチャしたいタイミングで — 毎フレームでなくてよい):
///   1. face 0..5 (+X, -X, +Y, -Y, +Z, -Z) を `render_pass()` +
///      `framebuffer(face)` で描く (90° FoV、 probe 位置から)。
///   2. `record_mip_generation(cmd)` — blit チェーンで mip を生成し、
///      全 mip を SHADER_READ_ONLY へ遷移する。
///   3. `cube_view()` + `probe_sampler()` を pbr_gi.frag (gi.glsl の
///      set 2 binding 4) へ結線し、 `GIGpuExecutor::set_env_params()` で
///      強度と mip 数を渡す。 roughness → mip LOD の粗い prefilter 近似
///      (本式の GGX prefilter は行わない — カジュアル向け)。
///
/// キャプチャしないホストは `initialize_fallback()` (1x1 黒) を使う —
/// pbr_gi.frag の binding 4 は常に有効な cubemap を要求するため。

#include "pictor/core/types.h"

#ifdef PICTOR_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace pictor {

class VulkanContext;

class GIReflectionProbe {
public:
    GIReflectionProbe() = default;
    ~GIReflectionProbe();

    GIReflectionProbe(const GIReflectionProbe&) = delete;
    GIReflectionProbe& operator=(const GIReflectionProbe&) = delete;

    bool is_initialized() const { return initialized_; }

#ifdef PICTOR_HAS_VULKAN
    /// キャプチャ用 probe を確保する (resolution² × 6 面、 mip 付き RGBA16F)。
    bool initialize(VulkanContext& vk, uint32_t resolution);

    /// 1x1 黒 cubemap (キャプチャ無しホストの binding 4 用フォールバック)。
    bool initialize_fallback(VulkanContext& vk);

    /// face 描画用 render pass (CLEAR → TRANSFER_SRC、 mip 0)。
    /// ホストは clearValueCount=1 で begin する。 fallback では VK_NULL_HANDLE。
    VkRenderPass  render_pass() const { return render_pass_; }
    VkFramebuffer framebuffer(uint32_t face) const {
        return face < 6 ? framebuffers_[face] : VK_NULL_HANDLE;
    }

    /// 6 面描画後に呼ぶ — mip チェーンを blit で生成し、 全体を
    /// SHADER_READ_ONLY へ遷移する。 fallback では不要 (no-op)。
    void record_mip_generation(VkCommandBuffer cmd);

    VkImageView cube_view() const     { return cube_view_; }
    VkSampler   probe_sampler() const { return sampler_; }
#endif

    uint32_t resolution() const { return resolution_; }
    uint32_t mip_count() const  { return mip_count_; }

    void shutdown();

private:
#ifdef PICTOR_HAS_VULKAN
    bool create_cubemap_(uint32_t resolution, uint32_t mips, bool renderable);

    VulkanContext* vk_     = nullptr;
    VkDevice       device_ = VK_NULL_HANDLE;

    VkImage        cube_image_  = VK_NULL_HANDLE;
    VkDeviceMemory cube_memory_ = VK_NULL_HANDLE;
    VkImageView    cube_view_   = VK_NULL_HANDLE;   // cubemap 全 mip
    VkImageView    face_views_[6] = {};             // mip 0 の各面 (fb 用)
    VkFramebuffer  framebuffers_[6] = {};
    VkRenderPass   render_pass_ = VK_NULL_HANDLE;
    VkSampler      sampler_     = VK_NULL_HANDLE;
#endif

    uint32_t resolution_  = 0;
    uint32_t mip_count_   = 1;
    bool     initialized_ = false;
};

} // namespace pictor
