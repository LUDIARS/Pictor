#pragma once

/// SHaRC ハイブリッド経路の G-buffer / 太陽シャドウマップ描画 (デモローカル)。
///
/// 120fps 試算 (1)(2) の実体:
///   (1) 一次可視性: SharcGpuExecutor のシーン SSBO を頂点プリングで
///       ラスタライズし、 RT0 albedo+AO / RT1 normal+rough / RT2 dist+MFP
///       の G-buffer を描く (compute BVH の一次レイ ~80-90ms → ~2-3ms)。
///   (2) 太陽影: 4096^2 ortho 深度マップの depth-only パス (~1-2ms)。
///
/// SRP: グラフィクスパイプラインと画像資源のみ。 G-buffer の消費
/// (shade/rays 生成) は executor の sharc_gbuffer_resolve.comp。

#ifdef PICTOR_HAS_VULKAN

#include <vulkan/vulkan.h>
#include <cstdint>

namespace pictor {
class VulkanContext;
}

namespace sharc_demo {

class GBufferRenderer {
public:
    GBufferRenderer() = default;
    ~GBufferRenderer();

    GBufferRenderer(const GBufferRenderer&) = delete;
    GBufferRenderer& operator=(const GBufferRenderer&) = delete;

    /// シーン SSBO (executor 所有、 葉順 SharcTri) を頂点プリングで参照する。
    bool initialize(pictor::VulkanContext& vk, const char* shader_dir,
                    uint32_t render_w, uint32_t render_h, uint32_t tri_count,
                    VkBuffer tris, VkDeviceSize tris_size,
                    VkBuffer tri_mats, VkDeviceSize tri_mats_size,
                    VkBuffer tri_ao, VkDeviceSize tri_ao_size,
                    VkBuffer materials, VkDeviceSize materials_size,
                    VkImageView atlas_view, VkSampler atlas_sampler);

    void shutdown();

    /// シャドウマップ + G-buffer を描画し、 compute 読み取り可能な状態
    /// (SHADER_READ_ONLY + バリア) にして戻る。 record() の前に呼ぶ。
    /// 行列は column-major mat4。
    void record(VkCommandBuffer cmd, const float* view_proj16,
                const float* sun_view_proj16, const float* camera_pos3);

    VkImageView albedo_ao_view() const   { return rt_[0].view; }
    VkImageView normal_rough_view() const { return rt_[1].view; }
    VkImageView dist_mfp_view() const    { return rt_[2].view; }
    VkImageView sun_shadow_view() const  { return shadow_.view; }

    bool is_initialized() const { return initialized_; }

    static constexpr uint32_t kShadowSize = 4096;

private:
    struct Image {
        VkImage        image = VK_NULL_HANDLE;
        VkDeviceMemory mem   = VK_NULL_HANDLE;
        VkImageView    view  = VK_NULL_HANDLE;
        VkFormat       format = VK_FORMAT_UNDEFINED;
    };

    struct PushParams {
        float view_proj[16];
        float camera_pos[4];
    };

    bool create_image_(Image& out, uint32_t w, uint32_t h, VkFormat format,
                       VkImageUsageFlags usage, VkImageAspectFlags aspect);
    void destroy_image_(Image& img);
    VkShaderModule load_shader_(const char* path);
    bool create_pipelines_(const char* shader_dir);

    pictor::VulkanContext* vk_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;

    uint32_t render_w_ = 0;
    uint32_t render_h_ = 0;
    uint32_t tri_count_ = 0;

    Image rt_[3];        // RT0 albedo+AO / RT1 normal+rough / RT2 dist+MFP
    Image depth_;        // G-buffer 深度 (D32)
    Image shadow_;       // 太陽シャドウマップ (D32, sampled)

    VkRenderPass  gbuffer_pass_ = VK_NULL_HANDLE;
    VkFramebuffer gbuffer_fb_   = VK_NULL_HANDLE;
    VkRenderPass  shadow_pass_  = VK_NULL_HANDLE;
    VkFramebuffer shadow_fb_    = VK_NULL_HANDLE;

    VkDescriptorSetLayout dsl_       = VK_NULL_HANDLE;
    VkPipelineLayout      layout_    = VK_NULL_HANDLE;
    VkDescriptorPool      desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet       desc_set_  = VK_NULL_HANDLE;
    VkPipeline gbuffer_pipeline_ = VK_NULL_HANDLE;
    VkPipeline shadow_pipeline_  = VK_NULL_HANDLE;

    bool initialized_ = false;
};

}  // namespace sharc_demo

#endif  // PICTOR_HAS_VULKAN
