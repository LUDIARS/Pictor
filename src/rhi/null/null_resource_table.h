#pragma once

#include "pictor/gfx/handle.h"
#include "pictor/gfx/pipeline_desc.h"
#include "pictor/gfx/resource_desc.h"
#include "rhi/common/handle_allocator.h"

#include <cstdint>
#include <vector>

namespace pictor::rhi {

/// Handle table of the null backend: issues generation-tagged handles and
/// keeps each resource's descriptor so tests can inspect what was created.
/// No GPU object exists behind any handle.
class NullResourceTable {
public:
    gfx::BufferId          create_buffer(const gfx::BufferDesc& desc);
    gfx::TextureId         create_texture(const gfx::TextureDesc& desc);
    gfx::SamplerId         create_sampler(const gfx::SamplerDesc& desc);
    gfx::PipelineId        create_pipeline(const gfx::PipelineDesc& desc);
    gfx::BindGroupId       create_bind_group();
    gfx::RenderTargetSetId create_render_target_set();

    bool destroy(gfx::BufferId id)          { return buffers_.destroy(id); }
    bool destroy(gfx::TextureId id)         { return textures_.destroy(id); }
    bool destroy(gfx::SamplerId id)         { return samplers_.destroy(id); }
    bool destroy(gfx::PipelineId id)        { return pipelines_.destroy(id); }
    bool destroy(gfx::BindGroupId id)       { return bind_groups_.destroy(id); }
    bool destroy(gfx::RenderTargetSetId id) { return target_sets_.destroy(id); }

    bool alive(gfx::BufferId id) const          { return buffers_.alive(id); }
    bool alive(gfx::TextureId id) const         { return textures_.alive(id); }
    bool alive(gfx::SamplerId id) const         { return samplers_.alive(id); }
    bool alive(gfx::PipelineId id) const        { return pipelines_.alive(id); }
    bool alive(gfx::BindGroupId id) const       { return bind_groups_.alive(id); }
    bool alive(gfx::RenderTargetSetId id) const { return target_sets_.alive(id); }

    /// Descriptor of a live resource, or nullptr for an invalid / stale handle.
    /// `debug_name` is not retained (the caller's string may not outlive it).
    const gfx::BufferDesc*   find(gfx::BufferId id) const   { return buffers_.find(id); }
    const gfx::TextureDesc*  find(gfx::TextureId id) const  { return textures_.find(id); }
    const gfx::SamplerDesc*  find(gfx::SamplerId id) const  { return samplers_.find(id); }
    const gfx::PipelineDesc* find(gfx::PipelineId id) const { return pipelines_.find(id); }

    uint32_t live_count() const noexcept;

private:
    struct NoDesc {};

    template <typename Tag, typename Desc>
    struct Slots {
        HandleAllocator   allocator;
        std::vector<Desc> descs;   ///< indexed by handle index, parallel to the allocator

        gfx::Handle<Tag> create(const Desc& desc) {
            const uint32_t bits = allocator.allocate();
            if (bits == 0) return {};
            const auto handle = gfx::Handle<Tag>::from_bits(bits);
            if (descs.size() <= handle.index()) descs.resize(handle.index() + 1);
            descs[handle.index()] = desc;
            return handle;
        }
        bool destroy(gfx::Handle<Tag> h) { return allocator.release(h.bits); }
        bool alive(gfx::Handle<Tag> h) const { return allocator.is_alive(h.bits); }
        const Desc* find(gfx::Handle<Tag> h) const {
            return allocator.is_alive(h.bits) ? &descs[h.index()] : nullptr;
        }
    };

    Slots<gfx::BufferTag, gfx::BufferDesc>          buffers_;
    Slots<gfx::TextureTag, gfx::TextureDesc>        textures_;
    Slots<gfx::SamplerTag, gfx::SamplerDesc>        samplers_;
    Slots<gfx::PipelineTag, gfx::PipelineDesc>      pipelines_;
    Slots<gfx::BindGroupTag, NoDesc>                bind_groups_;
    Slots<gfx::RenderTargetSetTag, NoDesc>          target_sets_;
};

} // namespace pictor::rhi
