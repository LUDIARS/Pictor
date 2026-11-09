// core/types.h (via the cache headers) comes before vulkan.h; see ui_renderer.cpp.
#include "pictor/vector_text/vector_text_renderer.h"

#ifdef PICTOR_HAS_VULKAN

#include "host_buffer.h"
#include "pictor/surface/vulkan_context.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace pictor {

using vector_text_detail::HostBuffer;

VectorTextRenderer::VectorTextRenderer() = default;
VectorTextRenderer::~VectorTextRenderer() { shutdown(); }

bool VectorTextRenderer::initialize(VulkanContext& vk, const std::string& shader_dir, VkRenderPass target_pass,
                                    uint32_t max_instances) {
    shutdown();
    vk_            = &vk;
    device_        = vk.device();
    max_instances_ = std::max<uint32_t>(max_instances, 1);
    vertices_ = std::make_unique<HostBuffer>(vk.physical_device(), device_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    indices_  = std::make_unique<HostBuffer>(vk.physical_device(), device_, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    for (uint32_t i = 0; i < vk.frames_in_flight(); ++i) {
        auto buffer = std::make_unique<HostBuffer>(vk.physical_device(), device_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (!buffer->allocate(sizeof(VectorTextInstance) * max_instances_)) {
            std::fprintf(stderr, "[vector_text] instance buffer allocation failed\n");
            shutdown();
            return false;
        }
        instances_.push_back(std::move(buffer));
    }
    if (!vertices_->allocate(sizeof(GlyphVertex) * 65536) || !indices_->allocate(sizeof(uint32_t) * 196608)) {
        std::fprintf(stderr, "[vector_text] mesh buffer allocation failed\n");
        shutdown();
        return false;
    }
    if (!create_pipeline_(shader_dir, target_pass)) {
        std::fprintf(stderr, "[vector_text] pipeline creation failed\n");
        shutdown();
        return false;
    }
    initialized_ = true;
    return true;
}

void VectorTextRenderer::shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (pipeline_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    }
    pipeline_        = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    instances_.clear();
    vertices_.reset();
    indices_.reset();
    uploaded_vertices_ = 0;
    uploaded_indices_  = 0;
    device_      = VK_NULL_HANDLE;
    vk_          = nullptr;
    initialized_ = false;
}

namespace {

/// Copies `count` items from `from` onward; regrows (losing the old content, so
/// everything is copied again) when the buffer is too small.
template <typename T>
bool upload_tail(VkDevice device, HostBuffer& buffer, const std::vector<T>& items, size_t& uploaded) {
    if (items.size() == uploaded) return true;
    const size_t need = items.size() * sizeof(T);
    if (need > buffer.capacity()) {
        vkDeviceWaitIdle(device);  // the old buffer may still be read by frames in flight
        if (!buffer.allocate(std::max(need, buffer.capacity() * 2))) return false;
        uploaded = 0;
    }
    std::memcpy(static_cast<T*>(buffer.mapped()) + uploaded, items.data() + uploaded,
                (items.size() - uploaded) * sizeof(T));
    uploaded = items.size();
    return true;
}

}  // namespace

bool VectorTextRenderer::sync(const GlyphMeshCache& cache) {
    if (!initialized_) return false;
    if (!upload_tail(device_, *vertices_, cache.vertices(), uploaded_vertices_) ||
        !upload_tail(device_, *indices_, cache.indices(), uploaded_indices_)) {
        std::fprintf(stderr, "[vector_text] mesh upload failed\n");
        return false;
    }
    return true;
}

void VectorTextRenderer::record(VkCommandBuffer cmd, VkExtent2D extent, const GlyphMeshCache& cache,
                                const VectorTextBatch& batch) {
    if (!initialized_ || batch.empty()) return;
    HostBuffer& slot = *instances_[vk_->current_frame() % instances_.size()];
    const size_t count = std::min<size_t>(batch.instances().size(), max_instances_);
    if (count < batch.instances().size() && !warned_overflow_) {
        std::fprintf(stderr, "[vector_text] %zu glyphs exceed max_instances %u; the rest are skipped\n",
                     batch.instances().size(), max_instances_);
        warned_overflow_ = true;
    }
    std::memcpy(slot.mapped(), batch.instances().data(), count * sizeof(VectorTextInstance));

    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                              0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const float view[2] = {static_cast<float>(extent.width), static_cast<float>(extent.height)};
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(view), view);
    const VkBuffer buffers[2] = {vertices_->buffer(), slot.buffer()};
    const VkDeviceSize offsets[2] = {0, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, indices_->buffer(), 0, VK_INDEX_TYPE_UINT32);

    for (const VectorTextDraw& draw : batch.draws()) {
        if (draw.first_instance >= count) break;
        const GlyphMeshRange& range = cache.range(draw.mesh);
        // Meshes built after the last sync() are not on the device yet.
        if (range.index_count == 0 || range.first_index + range.index_count > uploaded_indices_) continue;
        const uint32_t instances = static_cast<uint32_t>(std::min<size_t>(draw.instance_count, count - draw.first_instance));
        vkCmdDrawIndexed(cmd, range.index_count, instances, range.first_index,
                         static_cast<int32_t>(range.first_vertex), draw.first_instance);
    }
}

}  // namespace pictor

#endif  // PICTOR_HAS_VULKAN
