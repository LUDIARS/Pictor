#pragma once
/// VectorTextRenderer — draws a VectorTextBatch with Vulkan as instanced
/// triangle meshes (no glyph atlas, no SDF). Part of the optional
/// `Pictor::vector_text` module; spec/feature/vector-text-module.md.
#ifdef PICTOR_HAS_VULKAN

#include "pictor/vector_text/glyph_mesh_cache.h"
#include "pictor/vector_text/vector_text_batch.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace pictor {

class VulkanContext;

namespace vector_text_detail {
class HostBuffer;
}

class VectorTextRenderer {
public:
    VectorTextRenderer();
    ~VectorTextRenderer();
    VectorTextRenderer(const VectorTextRenderer&) = delete;
    VectorTextRenderer& operator=(const VectorTextRenderer&) = delete;

    /// `shader_dir` holds vector_text.vert.spv / vector_text.frag.spv.
    /// `target_pass` is the render pass (subpass 0) the text is drawn into.
    /// `max_instances` bounds the glyphs drawn per frame.
    bool initialize(VulkanContext& vk, const std::string& shader_dir, VkRenderPass target_pass,
                    uint32_t max_instances = 16384);
    void shutdown();
    [[nodiscard]] bool initialized() const { return initialized_; }

    /// Uploads meshes the cache built since the last call. Call once per frame
    /// before `record()`. Growing past the current buffer waits for the device
    /// to go idle (rare: the cache only grows when a new glyph or size appears).
    bool sync(const GlyphMeshCache& cache);

    /// Starts a frame: the instance space of the current flight slot is free again.
    /// Call once per frame before the first `record()`.
    void begin_frame() { written_ = 0; }

    /// Records the draws. The caller has begun `target_pass`; `batch.build_draws()`
    /// has been called. Uses the context's current flight slot for instances.
    /// May be called several times a frame (text interleaved with other draws):
    /// each batch's instances go behind the previous one's.
    void record(VkCommandBuffer cmd, VkExtent2D extent, const GlyphMeshCache& cache, const VectorTextBatch& batch);

private:
    bool create_pipeline_(const std::string& shader_dir, VkRenderPass pass);

    VulkanContext*   vk_              = nullptr;
    VkDevice         device_          = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline       pipeline_        = VK_NULL_HANDLE;
    uint32_t         max_instances_   = 0;
    bool             initialized_     = false;
    bool             warned_overflow_ = false;

    std::unique_ptr<vector_text_detail::HostBuffer>              vertices_;
    std::unique_ptr<vector_text_detail::HostBuffer>              indices_;
    std::vector<std::unique_ptr<vector_text_detail::HostBuffer>> instances_;  // one per flight slot
    size_t uploaded_vertices_ = 0;
    size_t written_ = 0;  // instances recorded into the current slot this frame
    size_t uploaded_indices_  = 0;
};

}  // namespace pictor

#endif  // PICTOR_HAS_VULKAN
