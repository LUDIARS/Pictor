/// 影絵デモ — 切り絵バックドロップテクスチャ。
///
/// Figmentum-kirie で生成した切り絵サンプル (街並み / 鷹) を PNG から読み、
/// 障子スクリーンの背景合成用に Vulkan テクスチャとして保持する。
///
///   - 街 (kirie_town.png)  : スクリーン全面の背景。不透明で読み込む。
///   - 鷹 (kirie_hawk.png)  : 台紙 (無地の紙地) を色距離でキーイングして
///                            アルファ化し、空へ重ねる切り抜きにする。
///
/// 読み込みに失敗した場合は 1x1 の白 (α=0) へフォールバックし、
/// デスクリプタは常に有効なまま本体演出だけが出る。
#pragma once

#include "pictor/surface/vulkan_context.h"

namespace sp_demo {

/// @implements SPEC-SHADOW-KIRIE-BACKDROP
class SpKirieBackdrop {
public:
    /// asset_dir 直下の kirie_town.png / kirie_hawk.png を読み込む。
    /// フォールバック生成も失敗したときのみ false。
    bool initialize(pictor::VulkanContext& vk_ctx, const char* asset_dir);
    void shutdown();

    VkSampler   sampler_handle() const { return sampler_; }
    VkImageView town_view() const { return town_.view; }
    VkImageView hawk_view() const { return hawk_.view; }

private:
    struct Texture {
        VkImage        image  = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView    view   = VK_NULL_HANDLE;
    };

    bool create_texture_rgba(const unsigned char* rgba, uint32_t width,
                             uint32_t height, Texture& out);
    bool load_png_texture(const char* path, bool key_background, Texture& out);
    void destroy_texture(Texture& tex);
    bool find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props,
                          uint32_t& memory_type) const;

    pictor::VulkanContext* vk_ctx_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;

    Texture   town_{};
    Texture   hawk_{};
    VkSampler sampler_ = VK_NULL_HANDLE;
};

} // namespace sp_demo
