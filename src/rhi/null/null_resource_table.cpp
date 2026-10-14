#include "rhi/null/null_resource_table.h"

namespace pictor::rhi {

gfx::BufferId NullResourceTable::create_buffer(const gfx::BufferDesc& desc) {
    gfx::BufferDesc kept = desc;
    kept.debug_name = nullptr;
    return buffers_.create(kept);
}

gfx::TextureId NullResourceTable::create_texture(const gfx::TextureDesc& desc) {
    gfx::TextureDesc kept = desc;
    kept.debug_name = nullptr;
    return textures_.create(kept);
}

gfx::SamplerId NullResourceTable::create_sampler(const gfx::SamplerDesc& desc) {
    return samplers_.create(desc);
}

gfx::PipelineId NullResourceTable::create_pipeline(const gfx::PipelineDesc& desc) {
    gfx::PipelineDesc kept = desc;
    kept.debug_name = nullptr;
    return pipelines_.create(kept);
}

gfx::BindGroupId NullResourceTable::create_bind_group() {
    return bind_groups_.create(NoDesc{});
}

gfx::RenderTargetSetId NullResourceTable::create_render_target_set() {
    return target_sets_.create(NoDesc{});
}

uint32_t NullResourceTable::live_count() const noexcept {
    return buffers_.allocator.live_count() + textures_.allocator.live_count() +
           samplers_.allocator.live_count() + pipelines_.allocator.live_count() +
           bind_groups_.allocator.live_count() + target_sets_.allocator.live_count();
}

} // namespace pictor::rhi
