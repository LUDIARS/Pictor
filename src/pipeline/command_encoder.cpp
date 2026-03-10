#include "pictor/pipeline/command_encoder.h"

namespace pictor {

void CommandEncoder::encode(const std::vector<RenderBatch>& batches,
                             PassType pass_type,
                             FrameAllocator& allocator) {
    for (const auto& batch : batches) {
        DrawCommand cmd;
        cmd.type = DrawCommand::Type::DRAW_INDEXED;
        cmd.index_count = batch.count;
        cmd.instance_count = 1;
        cmd.first_index = batch.startIndex;
        cmd.vertex_offset = 0;
        cmd.first_instance = 0;
        cmd.shader_key = batch.shaderKey;
        cmd.material_key = batch.materialKey;

        commands_.push_back(cmd);
        ++draw_call_count_;
        triangle_count_ += batch.count / 3; // approximate
    }
}

void CommandEncoder::encode_compute(uint32_t group_x, uint32_t group_y, uint32_t group_z,
                                     uint64_t shader_key) {
    DrawCommand cmd;
    cmd.type = DrawCommand::Type::DISPATCH_COMPUTE;
    cmd.group_count_x = group_x;
    cmd.group_count_y = group_y;
    cmd.group_count_z = group_z;
    cmd.shader_key = shader_key;
    commands_.push_back(cmd);
    ++draw_call_count_;
}

void CommandEncoder::encode_indirect(uint32_t indirect_offset, uint32_t max_draw_count,
                                      uint64_t shader_key) {
    DrawCommand cmd;
    cmd.type = DrawCommand::Type::DRAW_INDEXED_INDIRECT_COUNT;
    cmd.indirect_offset = indirect_offset;
    cmd.max_draw_count = max_draw_count;
    cmd.shader_key = shader_key;
    commands_.push_back(cmd);
    ++draw_call_count_;
}

} // namespace pictor
