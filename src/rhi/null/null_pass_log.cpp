#include "rhi/null/null_pass_log.h"

#include "rhi/null/null_resource_table.h"

namespace pictor::rhi {

namespace {

bool handle_alive(const gfx::PassCommand& c, const NullResourceTable& resources) {
    switch (c.op) {
        case gfx::PassOp::SetPipeline:
            return resources.alive(gfx::PipelineId::from_bits(c.handle));
        case gfx::PassOp::SetBindGroup:
            return resources.alive(gfx::BindGroupId::from_bits(c.handle));
        case gfx::PassOp::SetVertexBuffer:
        case gfx::PassOp::SetIndexBuffer:
            return resources.alive(gfx::BufferId::from_bits(c.handle));
        case gfx::PassOp::SetViewport:
        case gfx::PassOp::SetScissor:
        case gfx::PassOp::Draw:
        case gfx::PassOp::DrawIndexed:
        case gfx::PassOp::Dispatch:
        case gfx::PassOp::Count_:
            break;
    }
    return true;  // op carries no handle
}

} // namespace

const NullRecordedPass& NullPassLog::append(const gfx::PassRecorder& recorder,
                                            const gfx::PassContext& context,
                                            const NullResourceTable& resources) {
    NullRecordedPass pass;
    pass.context       = context;
    pass.first_command = static_cast<uint32_t>(commands_.size());
    pass.command_count = static_cast<uint32_t>(recorder.size());

    const gfx::PassCommand* cmds = recorder.data();
    for (size_t i = 0; i < recorder.size(); ++i) {
        if (!handle_alive(cmds[i], resources)) ++pass.stale_handle_count;
    }
    commands_.insert(commands_.end(), cmds, cmds + recorder.size());
    passes_.push_back(pass);
    return passes_.back();
}

} // namespace pictor::rhi
