#pragma once

#ifdef PICTOR_HAS_VULKAN

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace pictor {

class VulkanContext;

/// Bitmap font text renderer for debug overlays.
/// Renders monospace text using an embedded 8x16 VGA font atlas.
///
/// Usage per frame:
///   renderer.begin(cmd, extent);
///   renderer.draw_text(10, 10, "FPS: 60", 1,1,1);
///   renderer.end();   // single batched draw call
class BitmapTextRenderer {
public:
    BitmapTextRenderer() = default;
    ~BitmapTextRenderer();

    BitmapTextRenderer(const BitmapTextRenderer&) = delete;
    BitmapTextRenderer& operator=(const BitmapTextRenderer&) = delete;

    bool initialize(VulkanContext& vk_ctx, const char* shader_dir);
    void shutdown();

    /// Start a new text batch. Must be called inside an active render pass.
    void begin(VkCommandBuffer cmd, VkExtent2D screen_extent);

    /// Queue text for rendering. Coordinates in screen pixels (top-left origin).
    void draw_text(float x, float y, const char* text,
                   float r = 1.0f, float g = 1.0f, float b = 1.0f, float a = 1.0f);

    /// Flush all queued text as a single draw call.
    void end();

    /// Text scale multiplier (default 1.0 = 8x16 pixels per glyph).
    void set_scale(float s) { scale_ = s; }
    float scale() const { return scale_; }

private:
    struct TextVertex {
        float pos[2];
        float uv[2];
        float color[4];
    };

    bool create_font_texture();
    bool create_pipeline(const char* shader_dir);
    bool create_vertex_buffer();
    bool create_descriptor_sets();
    VkShaderModule load_shader(const char* path);
    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props);
    bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags props, VkBuffer& buf, VkDeviceMemory& mem);

    VulkanContext* vk_ctx_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;

    // Font texture
    VkImage font_image_ = VK_NULL_HANDLE;
    VkDeviceMemory font_image_memory_ = VK_NULL_HANDLE;
    VkImageView font_view_ = VK_NULL_HANDLE;
    VkSampler font_sampler_ = VK_NULL_HANDLE;

    // Pipeline
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet desc_set_ = VK_NULL_HANDLE;

    // Dynamic vertex buffer
    VkBuffer vertex_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertex_memory_ = VK_NULL_HANDLE;

    // Batch state
    std::vector<TextVertex> vertices_;
    VkCommandBuffer current_cmd_ = VK_NULL_HANDLE;
    VkExtent2D current_extent_ = {0, 0};

    float scale_ = 1.0f;

    static constexpr uint32_t MAX_CHARS = 4096;
    static constexpr uint32_t MAX_VERTICES = MAX_CHARS * 6;

    bool initialized_ = false;
};

} // namespace pictor

#endif // PICTOR_HAS_VULKAN
