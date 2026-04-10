#pragma once

#ifdef PICTOR_HAS_VULKAN

#include "pictor/ui/overlay_types.h"
#include "pictor/ui/screen_overlay_group.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <memory>
#include <string>
#include <unordered_map>

namespace pictor {

class VulkanContext;

/// Configuration for UIOverlayPipeline
struct UIOverlayConfig {
    uint32_t max_vertices   = 65536;   // max vertices across all groups
    uint32_t max_textures   = 32;      // max bound textures
    bool     enable_scissor = true;    // per-group scissor rect support
};

/// GPU-optimized UI rendering pipeline.
///
/// - Manages ScreenOverlayGroups with batched static vertices
/// - Single Vulkan pipeline with per-vertex color + texture sampling
/// - Groups sorted by sort_order, elements sorted by z_order within group
/// - Static elements are batched: vertex buffer updated only when dirty
/// - Push constants: orthographic projection matrix
///
/// Usage:
///   pipeline.initialize(vk_ctx, "shaders", config);
///   auto gid = pipeline.create_group("HUD");
///   pipeline.add_element(gid, elem);
///   // In render loop (inside render pass):
///   pipeline.update(screen_w, screen_h);  // rebuild dirty batches
///   pipeline.render(cmd, extent);          // record draw commands
class UIOverlayPipeline {
public:
    UIOverlayPipeline() = default;
    ~UIOverlayPipeline();

    UIOverlayPipeline(const UIOverlayPipeline&) = delete;
    UIOverlayPipeline& operator=(const UIOverlayPipeline&) = delete;

    // ─── Lifecycle ───────────────────────────────────────────

    bool initialize(VulkanContext& vk_ctx, const char* shader_dir,
                    const UIOverlayConfig& config = {});
    void shutdown();

    // ─── Group management ────────────────────────────────────

    OverlayGroupId create_group(const std::string& name, int32_t sort_order = 0);
    ScreenOverlayGroup* get_group(OverlayGroupId id);
    const ScreenOverlayGroup* get_group(OverlayGroupId id) const;
    bool remove_group(OverlayGroupId id);

    // ─── Convenience: element management via group ID ────────

    OverlayElementId add_element(OverlayGroupId group, const OverlayElement& elem);
    bool update_element(OverlayGroupId group, OverlayElementId elem_id,
                        const OverlayElement& elem);
    bool remove_element(OverlayGroupId group, OverlayElementId elem_id);

    // ─── Texture management ──────────────────────────────────

    /// Register a texture for use by overlay elements.
    /// Returns a TextureHandle (or INVALID_TEXTURE on failure).
    TextureHandle register_texture(const uint8_t* rgba_data, uint32_t width, uint32_t height,
                                    const std::string& name = "");

    /// Update an existing texture's pixel data.
    bool update_texture(TextureHandle handle, const uint8_t* rgba_data,
                        uint32_t width, uint32_t height);

    // ─── Rendering ───────────────────────────────────────────

    /// Rebuild dirty batches and upload vertex data. Call once per frame before render().
    void update(float screen_width, float screen_height);

    /// Record Vulkan draw commands for all visible groups.
    /// Must be called inside an active render pass.
    void render(VkCommandBuffer cmd, VkExtent2D extent);

    // ─── Stats ───────────────────────────────────────────────

    OverlayBatchStats stats() const { return stats_; }

    bool is_initialized() const { return initialized_; }

private:
    struct TextureEntry {
        VkImage        image  = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView    view   = VK_NULL_HANDLE;
        VkDescriptorSet desc_set = VK_NULL_HANDLE;
        uint32_t       width  = 0;
        uint32_t       height = 0;
        std::string    name;
    };

    bool create_pipeline(const char* shader_dir);
    bool create_vertex_buffer();
    bool create_sampler();
    bool create_descriptor_pool();
    VkShaderModule load_shader(const char* path);
    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props);
    bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags props, VkBuffer& buf, VkDeviceMemory& mem);
    bool upload_image(const uint8_t* data, uint32_t w, uint32_t h,
                      VkImage& image, VkDeviceMemory& memory);
    VkDescriptorSet allocate_texture_descriptor(VkImageView view);

    VulkanContext* vk_ctx_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    UIOverlayConfig config_;

    // Pipeline
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;

    // Shared sampler
    VkSampler sampler_ = VK_NULL_HANDLE;

    // Vertex buffer (dynamic, re-uploaded each frame if any group is dirty)
    VkBuffer vertex_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertex_memory_ = VK_NULL_HANDLE;
    VkDeviceSize vertex_buffer_size_ = 0;

    // Groups (sorted by sort_order for rendering)
    std::vector<std::unique_ptr<ScreenOverlayGroup>> groups_;
    OverlayGroupId next_group_id_ = 0;

    // Textures
    std::vector<TextureEntry> textures_;
    TextureHandle next_texture_handle_ = 0;

    // White 1x1 fallback texture
    TextureHandle white_texture_ = INVALID_TEXTURE;

    // Stats
    OverlayBatchStats stats_;

    bool initialized_ = false;
};

} // namespace pictor

#endif // PICTOR_HAS_VULKAN
