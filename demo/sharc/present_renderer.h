#pragma once

/// SHaRC present レンダラ (デモローカル)。
///
/// resolve の出力 SSBO をフラグメントシェーダが直読みして
/// スワップチェインへトーンマップ描画する。 CPU readback /
/// テクスチャ再アップロードを廃した全 GPU 経路の表示端。
///
/// SRP: フルスクリーン描画パイプラインと descriptor のみ。
/// バッファの所有は SharcGpuExecutor 側。

#ifdef PICTOR_HAS_VULKAN

#include <vulkan/vulkan.h>
#include <cstdint>

namespace pictor {
class VulkanContext;
}

namespace sharc_demo {

class PresentRenderer {
public:
    PresentRenderer() = default;
    ~PresentRenderer();

    PresentRenderer(const PresentRenderer&) = delete;
    PresentRenderer& operator=(const PresentRenderer&) = delete;

    /// output = resolve 出力 SSBO (SharcGpuExecutor::output_buffer())。
    bool initialize(pictor::VulkanContext& vk, const char* shader_dir,
                    VkBuffer output, VkDeviceSize output_size);

    void shutdown();

    /// render pass 内で呼ぶ。 レンダバッファ解像度と露出を push constant で渡す。
    /// albedo_mode = true でアルベド素通し (gamma のみ、 露出/Reinhard なし)。
    void render(VkCommandBuffer cmd, VkExtent2D extent, uint32_t render_w,
                uint32_t render_h, float exposure, bool albedo_mode = false);

    bool is_initialized() const { return initialized_; }

private:
    struct PushParams {
        uint32_t render_width;
        uint32_t render_height;
        float    exposure;
        float    albedo_mode;
    };

    VkShaderModule load_shader_(const char* path);

    pictor::VulkanContext* vk_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout dsl_       = VK_NULL_HANDLE;
    VkPipelineLayout      layout_    = VK_NULL_HANDLE;
    VkPipeline            pipeline_  = VK_NULL_HANDLE;
    VkDescriptorPool      desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet       desc_set_  = VK_NULL_HANDLE;

    bool initialized_ = false;
};

}  // namespace sharc_demo

#endif  // PICTOR_HAS_VULKAN
