#pragma once

#include "pictor/postprocess/postprocess_effect.h"
#include <cstdint>
#include <string>
#include <vector>

#ifdef PICTOR_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace pictor {

class VulkanContext;

/// Real, host-driven post-process pipeline.
///
/// The host renders its 3D scene into the pipeline-owned HDR target
/// (`scene_render_pass()` / `scene_framebuffer()`), then calls `record()`
/// to run the effect chain straight onto a swapchain image:
///
///   scene(HDR) → bloom extract → blur H → blur V
///              → final composite (bloom + tonemap + LUT + vignette) → output
///
/// All four effects always execute; a disabled effect collapses to an
/// identity via its push-constant parameters, so intermediate image
/// layouts stay valid every frame.
class PostProcessPipeline {
public:
    PostProcessPipeline();
    ~PostProcessPipeline();

    PostProcessPipeline(const PostProcessPipeline&) = delete;
    PostProcessPipeline& operator=(const PostProcessPipeline&) = delete;

    // ---- Config ----
    void set_config(const PostProcessConfig& config);
    const PostProcessConfig& config() const { return config_; }
    PostProcessConfig&       config_mut()    { return config_; }   ///< live tweaking

    bool is_initialized() const { return vulkan_ready_; }

#ifdef PICTOR_HAS_VULKAN
    /// Real Vulkan init. `output_views` are the swapchain image views the
    /// final pass renders into. `shader_dir` holds the compiled `.spv` files.
    /// `output_format` is the swapchain format.
    ///
    /// The LUT is supplied as host-decoded RGBA8 pixels (`lut_rgba`,
    /// `lut_w` x `lut_h`); pass nullptr for no LUT. Pictor stays free of any
    /// image-decoding dependency — the host loads `config.color_grading.lut_path`.
    /// Returns false on failure.
    bool initialize_vulkan(VulkanContext& vk,
                           const std::string& shader_dir,
                           uint32_t width, uint32_t height,
                           VkFormat output_format,
                           const std::vector<VkImageView>& output_views,
                           const PostProcessConfig& config,
                           const unsigned char* lut_rgba = nullptr,
                           int lut_w = 0, int lut_h = 0);

    /// Render pass / framebuffer the host renders its 3D scene into.
    /// rp_scene_ は color (RGBA16F) + depth (D32_SFLOAT) の 2 アタッチメント。
    /// ホストの vkCmdBeginRenderPass は clearValueCount=2 を渡すこと
    /// (clearValues[0]=color, clearValues[1]=depthStencil)。
    VkRenderPass  scene_render_pass() const { return rp_scene_; }
    VkFramebuffer scene_framebuffer() const { return fb_scene_; }
    VkExtent2D    extent() const { return extent_; }

    /// シーンカラー HDR ビュー (RGBA16F)。 DecalSystem 等がこれへ合成する。
    VkImageView   scene_color_view() const { return scene_.view; }
    /// シーンの深度ビュー (D32_SFLOAT)。 DecalSystem 等が read する。
    VkImageView   scene_depth_view() const { return scene_.depth_view; }

    /// Record the post-process chain into `output_views[output_index]`.
    /// The output image is left in COLOR_ATTACHMENT_OPTIMAL so the host can
    /// draw a HUD on top with a LOAD render pass before presenting.
    void record(VkCommandBuffer cmd, uint32_t output_index, float delta_time);

    /// Recreate size-dependent resources after a swapchain resize.
    /// Render passes / pipelines / descriptor pool / sampler / LUT are kept;
    /// HDR targets, framebuffers and descriptor writes are rebuilt.
    /// `output_views` are the new swapchain image views.
    bool resize(uint32_t width, uint32_t height,
                const std::vector<VkImageView>& output_views);
#endif

    void shutdown();

private:
    PostProcessConfig config_;
    uint32_t          width_  = 0;
    uint32_t          height_ = 0;
    bool              vulkan_ready_ = false;

#ifdef PICTOR_HAS_VULKAN
    struct RenderTarget {
        VkImage        image  = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView    view   = VK_NULL_HANDLE;
        VkFramebuffer  fb     = VK_NULL_HANDLE;
        // 深度アタッチメント (scene_ のみ使用。 ping/pong は VK_NULL_HANDLE)。
        VkImage        depth_image  = VK_NULL_HANDLE;
        VkDeviceMemory depth_memory = VK_NULL_HANDLE;
        VkImageView    depth_view   = VK_NULL_HANDLE;
    };
    struct Texture {
        VkImage        image  = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView    view   = VK_NULL_HANDLE;
    };

    bool create_render_passes_(VkFormat output_format);
    bool create_targets_();
    bool create_samplers_();
    bool create_descriptors_();
    void write_descriptor_sets_();
    bool create_pipelines_(const std::string& shader_dir);
    bool create_output_framebuffers_(const std::vector<VkImageView>& views);
    bool upload_lut_(const unsigned char* rgba, int w, int h);
    VkShaderModule load_shader_(const std::string& path) const;
    /// `with_depth` が true なら深度アタッチメント (D32_SFLOAT) も作る (scene_ 用)。
    bool create_rt_(RenderTarget& rt, VkFormat fmt, VkRenderPass rp, bool with_depth);
    uint32_t find_memory_type_(uint32_t filter, VkMemoryPropertyFlags props) const;

    VulkanContext* vk_     = nullptr;
    VkDevice       device_ = VK_NULL_HANDLE;
    VkExtent2D     extent_ = {0, 0};
    bool           output_is_srgb_ = false;

    VkRenderPass rp_scene_  = VK_NULL_HANDLE;  // RGBA16F + D32_SFLOAT, CLEAR
    VkRenderPass rp_inter_  = VK_NULL_HANDLE;  // RGBA16F, DONT_CARE
    VkRenderPass rp_output_ = VK_NULL_HANDLE;  // swapchain format, DONT_CARE

    RenderTarget scene_;   // host renders the 3D scene here (HDR)
    RenderTarget ping_;    // bloom intermediate
    RenderTarget pong_;    // bloom intermediate
    VkFramebuffer fb_scene_ = VK_NULL_HANDLE;  // alias of scene_.fb

    std::vector<VkFramebuffer> output_fbs_;    // one per swapchain image

    Texture   lut_{};
    bool      lut_loaded_ = false;
    VkSampler sampler_ = VK_NULL_HANDLE;

    VkDescriptorPool      desc_pool_   = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_single_  = VK_NULL_HANDLE;  // 1 sampler
    VkDescriptorSetLayout dsl_grade_   = VK_NULL_HANDLE;  // 3 samplers
    VkDescriptorSet       ds_extract_  = VK_NULL_HANDLE;  // samples scene_
    VkDescriptorSet       ds_blur_h_   = VK_NULL_HANDLE;  // samples ping_
    VkDescriptorSet       ds_blur_v_   = VK_NULL_HANDLE;  // samples pong_
    VkDescriptorSet       ds_grade_    = VK_NULL_HANDLE;  // scene_ + ping_ + lut_

    VkPipelineLayout pl_single_ = VK_NULL_HANDLE;
    VkPipelineLayout pl_grade_  = VK_NULL_HANDLE;
    VkPipeline       pipe_extract_ = VK_NULL_HANDLE;
    VkPipeline       pipe_blur_    = VK_NULL_HANDLE;
    VkPipeline       pipe_grade_   = VK_NULL_HANDLE;
#endif
};

} // namespace pictor
