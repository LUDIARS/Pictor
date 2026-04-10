#pragma once

#ifdef PICTOR_HAS_VULKAN

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace pictor {

class VulkanContext;

/// Push constants for 2D texture rendering (matches shader layout)
struct Texture2DPushConstants {
    float projection[16];
    float model[16];
    float tint[4];  // rgb + opacity
};

/// Self-contained 2D textured quad renderer for demos.
/// Creates its own pipeline, vertex buffer, texture, and descriptor sets.
class Texture2DRenderer {
public:
    Texture2DRenderer() = default;
    ~Texture2DRenderer();

    Texture2DRenderer(const Texture2DRenderer&) = delete;
    Texture2DRenderer& operator=(const Texture2DRenderer&) = delete;

    /// Initialize pipeline and quad vertex buffer.
    bool initialize(VulkanContext& vk_ctx, const char* shader_dir);

    void shutdown();

    /// Upload RGBA pixel data as a GPU texture. Can be called multiple times to switch textures.
    bool upload_texture(const uint8_t* rgba_data, uint32_t width, uint32_t height);

    /// Record draw commands into the given command buffer (must be inside a render pass).
    void render(VkCommandBuffer cmd, VkExtent2D extent, const Texture2DPushConstants& pc);

    bool is_initialized() const { return initialized_; }

private:
    struct Vertex2D {
        float pos[2];
        float uv[2];
    };

    bool create_pipeline(const char* shader_dir);
    bool create_quad_buffer();
    bool create_texture_resources();
    bool create_descriptor_sets();
    VkShaderModule load_shader(const char* path);
    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props);
    bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags props, VkBuffer& buf, VkDeviceMemory& mem);

    VulkanContext* vk_ctx_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;

    // Pipeline
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet desc_set_ = VK_NULL_HANDLE;

    // Quad vertex buffer
    VkBuffer vertex_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertex_memory_ = VK_NULL_HANDLE;

    // Texture
    VkImage texture_image_ = VK_NULL_HANDLE;
    VkDeviceMemory texture_memory_ = VK_NULL_HANDLE;
    VkImageView texture_view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    uint32_t tex_width_ = 0;
    uint32_t tex_height_ = 0;
    bool texture_uploaded_ = false;

    bool initialized_ = false;
};

} // namespace pictor

#endif // PICTOR_HAS_VULKAN
