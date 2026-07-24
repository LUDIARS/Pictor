#pragma once

/// SHaRC ポストプロセス: Bloom パイプライン (デモローカル)。
///
/// resolve の HDR 出力 SSBO を入力に、 半解像度の mip チェーンで
///   extract (ソフトニー閾値 + Karis average)
///   → 13-tap ダウンサンプル ×N
///   → テントフィルタ アップサンプル加算 ×N
/// を compute で回す。 up[0] (半解像度 rgba16f) を PresentRenderer が
/// トーンマップ前に加算合成する。 コスト ~0.5ms @720p/1070。
///
/// SRP: bloom の compute パスと画像資源のみ。 合成は present 側。

#ifdef PICTOR_HAS_VULKAN

#include <vulkan/vulkan.h>
#include <cstdint>

namespace pictor {
class VulkanContext;
}

namespace sharc_demo {

class BloomPipeline {
public:
    BloomPipeline() = default;
    ~BloomPipeline();

    BloomPipeline(const BloomPipeline&) = delete;
    BloomPipeline& operator=(const BloomPipeline&) = delete;

    bool initialize(pictor::VulkanContext& vk, const char* shader_dir,
                    uint32_t render_w, uint32_t render_h,
                    VkBuffer hdr_output, VkDeviceSize hdr_size);

    void shutdown();

    /// bloom チェーンを記録する。 呼び出し側は事前に resolve 出力 SSBO の
    /// compute→compute バリアを張ること。 終了時に up[0] は
    /// SHADER_READ (fragment) 可視。
    void record(VkCommandBuffer cmd, float exposure);

    /// present の合成入力 (up[0]、 半解像度)。
    VkImageView result_view() const { return up_views_[0]; }
    VkSampler   result_sampler() const { return sampler_; }

    bool is_initialized() const { return initialized_; }

    static constexpr uint32_t kMipCount = 5;   // 640x360 → 40x22 @720p

private:
    struct PushParams {
        uint32_t render_w;
        uint32_t render_h;
        float    exposure;
        float    threshold;
        float    knee;
    };

    VkShaderModule load_shader_(const char* path);
    bool create_pipeline_(const char* shader_dir, const char* name,
                          VkPipelineLayout layout, VkPipeline* out);

    pictor::VulkanContext* vk_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;

    uint32_t mip_w_[kMipCount] = {};
    uint32_t mip_h_[kMipCount] = {};

    // down / up の 2 チェーン (各 kMipCount 枚の独立 rgba16f 画像)
    VkImage        down_imgs_[kMipCount] = {};
    VkImage        up_imgs_[kMipCount]   = {};
    VkDeviceMemory down_mem_[kMipCount]  = {};
    VkDeviceMemory up_mem_[kMipCount]    = {};
    VkImageView    down_views_[kMipCount] = {};
    VkImageView    up_views_[kMipCount]   = {};
    VkSampler      sampler_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout dsl_extract_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_down_    = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_up_      = VK_NULL_HANDLE;
    VkPipelineLayout layout_extract_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_down_    = VK_NULL_HANDLE;
    VkPipelineLayout layout_up_      = VK_NULL_HANDLE;
    VkPipeline pipe_extract_ = VK_NULL_HANDLE;
    VkPipeline pipe_down_    = VK_NULL_HANDLE;
    VkPipeline pipe_up_      = VK_NULL_HANDLE;

    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_extract_ = VK_NULL_HANDLE;
    VkDescriptorSet set_down_[kMipCount]  = {};   // [i]: down[i-1]→down[i]
    VkDescriptorSet set_up_[kMipCount]    = {};   // [i]: up[i+1]+down[i]→up[i]

    bool first_frame_ = true;
    bool initialized_ = false;
};

}  // namespace sharc_demo

#endif  // PICTOR_HAS_VULKAN
