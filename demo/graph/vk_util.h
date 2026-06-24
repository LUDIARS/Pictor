#pragma once

#ifdef PICTOR_HAS_VULKAN

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace pictor {

class VulkanContext;

namespace graph {

/// Shared Vulkan helpers for the demo's instanced renderers (graph + dock UI).
/// Keeps the boilerplate in one place so each renderer only describes what is
/// unique to it (its shaders, push constants and instance layout).

VkShaderModule load_shader_module(VkDevice device, const char* spv_path);

uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t type_filter,
                          VkMemoryPropertyFlags props);

/// Standard 2D pipeline used by every demo instanced renderer:
///   - no vertex input (geometry from gl_VertexIndex)
///   - triangle list, no cull, no depth
///   - straight alpha blend
///   - dynamic viewport + scissor
/// The caller owns `layout` (push constant + descriptor layout) and `render_pass`.
VkPipeline create_instanced_pipeline(VkDevice device, VkRenderPass render_pass,
                                     VkPipelineLayout layout,
                                     const char* vert_spv, const char* frag_spv);

/// Per-frame-in-flight, host-visible, persistently-mapped storage buffers plus a
/// descriptor set each, for instance data that is rebuilt every frame (cull /
/// dock layout). Rotating by flight index makes CPU writes safe against the GPU
/// still reading an earlier frame (the acquire fence gates reuse).
class DynamicInstanceBuffers {
public:
    bool initialize(VulkanContext& vk_ctx, uint32_t flights, VkDeviceSize capacity_bytes,
                    VkDescriptorSetLayout set_layout, VkDescriptorPool pool);
    void destroy(VkDevice device);

    /// Copy `bytes` of instance data into the given flight's mapped buffer.
    void upload(uint32_t flight, const void* data, VkDeviceSize bytes);

    VkDescriptorSet set(uint32_t flight) const { return sets_[flight]; }
    VkDeviceSize    capacity() const { return capacity_; }

private:
    std::vector<VkBuffer>        buffers_;
    std::vector<VkDeviceMemory>  memories_;
    std::vector<void*>           mapped_;
    std::vector<VkDescriptorSet> sets_;
    VkDeviceSize                 capacity_ = 0;
};

} // namespace graph
} // namespace pictor

#endif // PICTOR_HAS_VULKAN
