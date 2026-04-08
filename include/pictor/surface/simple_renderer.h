#pragma once

#ifdef PICTOR_HAS_VULKAN

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace pictor {

class VulkanContext;

/// Minimal instanced renderer for demos and benchmarks.
/// Renders icosphere instances with per-instance position+scale from a storage buffer.
class SimpleRenderer {
public:
    SimpleRenderer() = default;
    ~SimpleRenderer();

    SimpleRenderer(const SimpleRenderer&) = delete;
    SimpleRenderer& operator=(const SimpleRenderer&) = delete;

    /// Initialize pipeline, mesh buffers, and descriptor sets.
    /// shader_dir: directory containing simple_inst.vert.spv / simple_inst.frag.spv
    bool initialize(VulkanContext& vk_ctx, const char* shader_dir);

    void shutdown();

    /// Upload instance data (vec4 per instance: xyz=position, w=scale).
    /// Call before render() each frame.
    void update_instances(const float* data, uint32_t count);

    /// Record draw commands into the given command buffer.
    /// view and proj are column-major 4x4 matrices.
    void render(VkCommandBuffer cmd, VkRenderPass render_pass, VkFramebuffer framebuffer,
                VkExtent2D extent, const float* view, const float* proj);

private:
    struct Vertex {
        float pos[3];
        float normal[3];
    };

    bool create_pipeline(const char* shader_dir);
    bool create_mesh_buffers();
    bool create_instance_buffer();
    bool create_descriptor_sets();
    VkShaderModule load_shader(const char* path);
    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props);
    bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags props, VkBuffer& buf, VkDeviceMemory& mem);

    void generate_icosphere(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices);

    VulkanContext* vk_ctx_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;

    // Pipeline
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet desc_set_ = VK_NULL_HANDLE;

    // Mesh
    VkBuffer vertex_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertex_memory_ = VK_NULL_HANDLE;
    VkBuffer index_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory index_memory_ = VK_NULL_HANDLE;
    uint32_t index_count_ = 0;

    // UBO
    VkBuffer ubo_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory ubo_memory_ = VK_NULL_HANDLE;

    // Instance SSBO
    VkBuffer instance_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory instance_memory_ = VK_NULL_HANDLE;
    VkDeviceSize instance_buffer_size_ = 0;
    uint32_t instance_count_ = 0;

    static constexpr uint32_t MAX_INSTANCES = 1100000;

    bool initialized_ = false;
};

} // namespace pictor

#endif // PICTOR_HAS_VULKAN
