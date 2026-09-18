#pragma once

#include "pictor/core/types.h"
#include <cstdint>
#include <string>
#include <vector>

#ifdef PICTOR_HAS_VULKAN
#ifndef NOGDI
#define NOGDI
#endif
#include <vulkan/vulkan.h>
#endif

namespace pictor::xr {

#ifdef PICTOR_HAS_VULKAN

/// 再投影元の画。 view は host 所有で、 記録時には
/// 色 = SHADER_READ_ONLY_OPTIMAL、 深度 = DEPTH_STENCIL_READ_ONLY_OPTIMAL であること。
struct ReprojectionSource {
    VkImageView color  = VK_NULL_HANDLE;
    VkImageView depth  = VK_NULL_HANDLE;
    uint32_t    width  = 0;
    uint32_t    height = 0;
};

struct ReprojectionPassDesc {
    VkDevice     device      = VK_NULL_HANDLE;
    /// 写し先の render pass (色 1 + 深度 1)。 深度は書き込み可能であること。
    VkRenderPass render_pass = VK_NULL_HANDLE;
    uint32_t     subpass     = 0;
    /// reproject_warp.vert.spv / .frag.spv があるディレクトリ。
    std::string  shader_dir;
    /// 同時に登録できる再投影元の数 (両眼 × キーフレームなら 2)。
    uint32_t     max_sources = 4;
};

struct ReprojectionParams {
    uint32_t dst_width      = 0;
    uint32_t dst_height     = 0;
    /// 格子 1 マスの大きさ (元の画の画素数)。 小さいほど輪郭が正確で頂点が増える。
    uint32_t grid_cell_px   = 4;
    /// 写し先 1 画素あたり元の画をこの画素数未満しか進まない三角形は穴にする。
    float    min_texel_rate = 0.5f;
    /// 写した画素を手前へずらす量 (NDC 深度)。 後段の穴埋めを LESS で弾くため。
    float    depth_bias     = 1e-5f;
};

/// 描画済みの色 + 深度を別の視点へ写すパス。 両眼差分と時間差分の共通部品。
///
/// 写した後、 host は同じ render pass のまま静的な物体を深度テスト LESS で描く。
/// 写せた画素は深度で弾かれ、 穴の画素だけがシェーディングされる。
///
/// 資源の所有: pipeline / descriptor / sampler は本クラスが持ち、 shutdown() か
/// デストラクタで解放する。 VkDevice と再投影元の view は借用。
class ReprojectionPass {
public:
    static constexpr uint32_t kInvalidSlot = UINT32_MAX;

    ReprojectionPass() = default;
    ~ReprojectionPass();

    ReprojectionPass(const ReprojectionPass&)            = delete;
    ReprojectionPass& operator=(const ReprojectionPass&) = delete;

    bool initialize(const ReprojectionPassDesc& desc);
    void shutdown();
    bool is_initialized() const { return pipeline_ != VK_NULL_HANDLE; }

    /// 再投影元を登録して slot を得る。 空きが無い・view が無効なら kInvalidSlot。
    uint32_t register_source(const ReprojectionSource& source);

    /// 登録済み slot の view を差し替える (描画先を作り直した後)。
    /// その slot を参照するコマンドが GPU 上で走っていない時に呼ぶこと。
    bool update_source(uint32_t slot, const ReprojectionSource& source);

    /// render pass の中で呼ぶ。 viewport / scissor は写し先の全面に設定する。
    /// `dst_clip_from_src_ndc` は build_reprojection() の結果。
    void record(VkCommandBuffer cmd, uint32_t slot,
                const float4x4& dst_clip_from_src_ndc,
                const ReprojectionParams& params) const;

private:
    bool create_descriptor_objects_(uint32_t max_sources);
    bool create_pipeline_(const ReprojectionPassDesc& desc);
    void write_source_(uint32_t slot, const ReprojectionSource& source);

    VkDevice              device_          = VK_NULL_HANDLE;
    VkSampler             color_sampler_   = VK_NULL_HANDLE;
    VkSampler             depth_sampler_   = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_      = VK_NULL_HANDLE;
    VkDescriptorPool      descriptor_pool_ = VK_NULL_HANDLE;
    VkPipelineLayout      pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline            pipeline_        = VK_NULL_HANDLE;

    // slot で引く平坦な配列。 登録は初期化時だけで、 記録時は添字参照のみ。
    std::vector<VkDescriptorSet>    sets_;
    std::vector<ReprojectionSource> sources_;
    uint32_t                        used_slots_ = 0;
};

#endif // PICTOR_HAS_VULKAN

} // namespace pictor::xr
