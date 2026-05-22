#pragma once

/// UIRenderer — ergo_ui_kit が生成する描画リストを Vulkan で描く即時モードの
/// UI レンダラ (ui_framework.md P2)。
///
/// ergo_ui_kit の `DrawCmd` と構造的に等価な `UIDrawCmd` を受け取る。 Pictor は
/// ergo に依存しないため、 ホスト (ゲーム) が ergo の DrawCmd → UIDrawCmd へ
/// 変換して渡す (両者は同型なので変換は自明)。
///
/// 担当: Rect (角丸) / Image (テクスチャ + tint) / NineSlice / PushClip /
/// PopClip (scissor)。 Text はホストの既存テキストレンダラに委ねる
/// (UIRenderer は Text コマンドを無視する)。

#ifdef PICTOR_HAS_VULKAN

#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace pictor {

class VulkanContext;

enum class UIDrawKind : uint8_t {
    Rect = 0, NineSlice = 1, Image = 2, Text = 3, PushClip = 4, PopClip = 5,
};

/// ergo_ui_kit::DrawCmd と同型の描画コマンド (Pictor 側の型)。
struct UIDrawCmd {
    UIDrawKind kind = UIDrawKind::Rect;
    float      x = 0, y = 0, w = 0, h = 0;     // 画面 px
    float      r = 1, g = 1, b = 1, a = 1;     // fill / tint
    float      corner_radius = 0.0f;
    uint32_t   texture_id    = 0;              // 0 = テクスチャ無し (純色)
    float      nine[4]       = {0, 0, 0, 0};   // 9-slice insets (px)
    float      opacity       = 1.0f;
};

class UIRenderer {
public:
    UIRenderer() = default;
    ~UIRenderer();

    UIRenderer(const UIRenderer&) = delete;
    UIRenderer& operator=(const UIRenderer&) = delete;

    /// `target_pass` は UI を描き込む render pass (HUD パス等)。
    bool initialize(VulkanContext& vk, const std::string& shader_dir,
                    VkRenderPass target_pass);
    void shutdown();
    bool initialized() const { return initialized_; }

    /// テクスチャを登録して `texture_id` を得る。 id 0 は内蔵の白テクスチャ
    /// (純色 Rect 用)。 登録上限は内部の max_textures。
    uint32_t register_texture(VkImageView view);

    /// 描画リストを記録する。 呼び出し側が render pass を begin した状態で呼ぶ。
    void record(VkCommandBuffer cmd, VkExtent2D extent,
                const std::vector<UIDrawCmd>& cmds);

private:
    VkShaderModule load_shader_(const std::string& path) const;
    uint32_t       find_memory_type_(uint32_t filter, VkMemoryPropertyFlags props) const;
    bool           create_white_texture_();
    bool           create_descriptors_();
    bool           create_pipeline_(const std::string& shader_dir, VkRenderPass rp);
    VkDescriptorSet alloc_texture_set_(VkImageView view);

    VulkanContext* vk_     = nullptr;
    VkDevice       device_ = VK_NULL_HANDLE;

    static constexpr uint32_t kMaxTextures = 64;

    VkSampler sampler_ = VK_NULL_HANDLE;

    // 内蔵 1x1 白テクスチャ (純色 Rect 用)。
    VkImage        white_image_  = VK_NULL_HANDLE;
    VkDeviceMemory white_memory_ = VK_NULL_HANDLE;
    VkImageView    white_view_   = VK_NULL_HANDLE;

    VkDescriptorPool      desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_tex_   = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> texture_sets_;   // index = texture_id

    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline       pipeline_        = VK_NULL_HANDLE;

    bool initialized_ = false;
};

} // namespace pictor

#endif // PICTOR_HAS_VULKAN
