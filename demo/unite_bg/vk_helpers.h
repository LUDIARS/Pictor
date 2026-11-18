#pragma once
// =============================================================================
// Unite BG デモ専用の Vulkan 補助 (バッファ / 画像 / オフスクリーン / パイプライン)
// Pictor の VulkanContext (instance/device/swapchain) の上に載せる。
// =============================================================================

#ifdef PICTOR_HAS_VULKAN

#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <vector>

namespace pictor { class VulkanContext; }

namespace ubg {

VkShaderModule load_shader(VkDevice device, const std::string& path);
uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t filter, VkMemoryPropertyFlags props);

struct Buffer {
    VkBuffer       buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void*          mapped = nullptr;
    VkDeviceSize   size   = 0;
    bool create(pictor::VulkanContext& ctx, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible);
    void destroy(VkDevice device);
};

struct Image {
    VkImage        image  = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView    view   = VK_NULL_HANDLE;
    VkFormat       format = VK_FORMAT_UNDEFINED;
    uint32_t       width = 0, height = 0;
    bool create(pictor::VulkanContext& ctx, uint32_t w, uint32_t h, VkFormat fmt,
                VkImageUsageFlags usage, VkImageAspectFlags aspect);
    void destroy(VkDevice device);
};

/// 1 回 (ベイク) または毎フレーム描くオフスクリーン。カラーのみ・MRT 対応。
/// 描き終わると SHADER_READ_ONLY になり、そのままサンプルできる。
struct OffscreenTarget {
    std::vector<Image> colors;
    VkRenderPass  render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint32_t      width = 0, height = 0;
    bool create(pictor::VulkanContext& ctx, uint32_t w, uint32_t h, uint32_t count, VkFormat fmt);
    void destroy(VkDevice device);
    void begin(VkCommandBuffer cmd) const;
    void end(VkCommandBuffer cmd) const { vkCmdEndRenderPass(cmd); }
    /// 作成直後のレイアウトを SHADER_READ_ONLY にしておく (未描画でもサンプル可能に)
    void transition_to_read(pictor::VulkanContext& ctx) const;
};

struct GfxState {
    bool        depth_test  = false;
    bool        depth_write = false;
    VkCompareOp depth_op    = VK_COMPARE_OP_LESS_OR_EQUAL;
    uint32_t    color_count = 1;
    bool        has_depth_attachment = true;
};

VkPipeline create_graphics_pipeline(VkDevice device, VkRenderPass rp, VkPipelineLayout layout,
                                    const std::string& vert_spv, const std::string& frag_spv,
                                    const GfxState& st);
VkPipeline create_compute_pipeline(VkDevice device, VkPipelineLayout layout, const std::string& comp_spv);

} // namespace ubg

#endif
