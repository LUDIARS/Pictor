#pragma once

#ifdef PICTOR_HAS_VULKAN

#include <vulkan/vulkan.h>
#include <cstdint>

#include "graph_instances.h"
#include "vk_util.h"

namespace pictor {

class VulkanContext;

namespace graph {

struct UiPushConstants {
    float proj[16];   // screen px -> clip
};

/// Instanced solid-rect renderer for the dock chrome (panel backgrounds,
/// splitter handles, tab headers). Screen-space; rebuilt each frame into a
/// per-flight dynamic buffer (counts are tiny).
class UiRectRenderer {
public:
    UiRectRenderer() = default;
    ~UiRectRenderer();

    UiRectRenderer(const UiRectRenderer&)            = delete;
    UiRectRenderer& operator=(const UiRectRenderer&) = delete;

    bool initialize(VulkanContext& vk_ctx, const char* shader_dir, uint32_t capacity);
    void shutdown();

    void draw(VkCommandBuffer cmd, VkExtent2D extent, const float proj[16],
              uint32_t flight, const UiRect* rects, uint32_t count);

private:
    VulkanContext* vk_ctx_ = nullptr;
    VkDevice       device_ = VK_NULL_HANDLE;
    uint32_t       flights_ = 1;

    VkDescriptorSetLayout desc_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool      desc_pool_       = VK_NULL_HANDLE;
    VkPipelineLayout      pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline            pipeline_        = VK_NULL_HANDLE;

    DynamicInstanceBuffers buf_;
    bool initialized_ = false;
};

} // namespace graph
} // namespace pictor

#endif // PICTOR_HAS_VULKAN
