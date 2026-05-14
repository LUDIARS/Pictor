#pragma once

/// 投影デカール (projected decal) システム。
///
/// シーン深度からワールド座標を再構成し、 デカールの OBB (有向境界ボックス)
/// 内かどうかを判定して、 ボックスのローカル UV でテクスチャをサンプルし
/// シーンカラー (HDR) へ合成する。 弾痕・着弾痕・スキル範囲表示など、
/// ジオメトリを足さずにサーフェスへ模様を貼る用途。
///
/// 使い方:
///   scene pass (color + depth) → DecalSystem::record() → post-process
///
/// `record()` はシーンカラー HDR ターゲットへ直接ブレンド合成する。 深度は
/// サンプルするだけ (read only)。 P2 では ALPHA ブレンド + フルスクリーン
/// 三角形描画のみ (decals.md の段階実装参照)。

#include <cstdint>
#include <string>
#include <vector>

#ifdef PICTOR_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace pictor {

class VulkanContext;

using DecalHandle = uint32_t;
constexpr DecalHandle INVALID_DECAL = 0;

/// 合成ブレンドモード (P2 は Alpha のみ実装、 P3 で Additive/Multiply)。
enum class DecalBlend : uint8_t {
    Alpha    = 0,
    Additive = 1,
    Multiply = 2,
};

/// 1 デカールの記述子。 OBB はローカル `[-0.5,0.5]^3` の単位ボックスを
/// `world_transform` (column-major / column-vector, 平行移動が m[12..14] —
/// KuzuSurvivors の mat_trs 等と同じ規約) で変換したもの。 投影は OBB の
/// ローカル Y 軸方向、 ローカル XZ が UV。
struct DecalDescriptor {
    float       world_transform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
#ifdef PICTOR_HAS_VULKAN
    VkImageView texture = VK_NULL_HANDLE;   ///< デカール画像 (RGBA, ホストが用意)
#endif
    DecalBlend  blend       = DecalBlend::Alpha;
    float       opacity     = 1.0f;
    float       angle_fade  = 0.35f;        ///< (P3) 投影面の角度フェード
    float       depth_fade  = 0.08f;        ///< OBB の Y 端でのフェード幅
    uint32_t    sort_order  = 0;            ///< 同位置の重ね順 (小→先に描画)
};

class DecalSystem {
public:
    DecalSystem() = default;
    ~DecalSystem();

    DecalSystem(const DecalSystem&) = delete;
    DecalSystem& operator=(const DecalSystem&) = delete;

#ifdef PICTOR_HAS_VULKAN
    /// `scene_color_view` はデカールを合成する HDR ターゲット (scene pass が
    /// 描いたもの)。 `scene_depth_view` はそのシーン深度。 両 view は scene pass
    /// の finalLayout (color=SHADER_READ_ONLY / depth=DEPTH_STENCIL_READ_ONLY)
    /// にあることを前提とする。
    bool initialize(VulkanContext& vk, const std::string& shader_dir,
                    VkFormat scene_color_format,
                    VkImageView scene_color_view,
                    VkImageView scene_depth_view,
                    VkExtent2D extent);

    /// スワップチェーン再生成後、 view が作り直されたら呼ぶ。
    bool resize(VkImageView scene_color_view, VkImageView scene_depth_view,
                VkExtent2D extent);

    /// scene pass 後・ post-process 前に呼ぶ。 `view` / `proj` は scene を
    /// 描画したカメラ行列 (column-major)。 深度からワールド座標を復元する
    /// 逆行列は内部で計算する。
    void record(VkCommandBuffer cmd, const float view[16], const float proj[16]);
#endif

    void shutdown();

    DecalHandle add(const DecalDescriptor& d);
    void        update(DecalHandle h, const DecalDescriptor& d);
    void        remove(DecalHandle h);
    void        clear();

    int  count() const;
    bool initialized() const { return initialized_; }

private:
#ifdef PICTOR_HAS_VULKAN
    struct DecalEntry {
        DecalDescriptor desc;
        VkDescriptorSet tex_set = VK_NULL_HANDLE;  // set 1: decal texture
        bool            alive   = false;
    };

    bool create_render_pass_(VkFormat color_format);
    bool create_framebuffer_();
    bool create_descriptors_();
    bool create_pipeline_(const std::string& shader_dir);
    VkShaderModule load_shader_(const std::string& path) const;
    uint32_t find_memory_type_(uint32_t filter, VkMemoryPropertyFlags props) const;
    VkDescriptorSet alloc_texture_set_(VkImageView texture);

    VulkanContext* vk_     = nullptr;
    VkDevice       device_ = VK_NULL_HANDLE;
    VkExtent2D     extent_ = {0, 0};

    VkImageView   scene_color_view_ = VK_NULL_HANDLE;  // borrowed
    VkImageView   scene_depth_view_ = VK_NULL_HANDLE;  // borrowed
    VkRenderPass  render_pass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkBuffer       ubo_buf_ = VK_NULL_HANDLE;   // inv_view_proj
    VkDeviceMemory ubo_mem_ = VK_NULL_HANDLE;
    void*          ubo_mapped_ = nullptr;

    VkDescriptorPool      desc_pool_  = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_scene_  = VK_NULL_HANDLE;  // set 0: UBO + depth
    VkDescriptorSetLayout dsl_decal_  = VK_NULL_HANDLE;  // set 1: decal texture
    VkDescriptorSet       ds_scene_   = VK_NULL_HANDLE;

    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline       pipeline_alpha_  = VK_NULL_HANDLE;

    std::vector<DecalEntry> decals_;
    std::vector<uint32_t>   free_slots_;
#endif
    bool initialized_ = false;
};

} // namespace pictor
