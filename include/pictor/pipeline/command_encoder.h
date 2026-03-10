#pragma once

#include "pictor/core/types.h"
#include "pictor/batch/batch_builder.h"
#include "pictor/memory/frame_allocator.h"
#include <vector>

namespace pictor {

/// Draw command for command buffer recording
struct DrawCommand {
    enum class Type : uint8_t {
        DRAW_INDEXED,
        DRAW_INDEXED_INDIRECT,
        DRAW_INDEXED_INDIRECT_COUNT,
        DISPATCH_COMPUTE
    };

    Type     type;
    uint32_t index_count     = 0;
    uint32_t instance_count  = 1;
    uint32_t first_index     = 0;
    int32_t  vertex_offset   = 0;
    uint32_t first_instance  = 0;
    uint64_t shader_key      = 0;
    uint32_t material_key    = 0;

    // For indirect/compute
    uint32_t indirect_offset = 0;
    uint32_t max_draw_count  = 0;
    uint32_t group_count_x   = 0;
    uint32_t group_count_y   = 1;
    uint32_t group_count_z   = 1;
};

/// Command encoder: generates DrawCommands from batches and records to buffer (§2.2).
/// All temporary data allocated from Frame Allocator.
class CommandEncoder {
public:
    CommandEncoder() = default;
    ~CommandEncoder() = default;

    /// Encode draw commands from batches for a specific pass
    void encode(const std::vector<RenderBatch>& batches,
                PassType pass_type,
                FrameAllocator& allocator);

    /// Encode a compute dispatch
    void encode_compute(uint32_t group_x, uint32_t group_y, uint32_t group_z,
                        uint64_t shader_key);

    /// Encode indirect draw (GPU Driven)
    void encode_indirect(uint32_t indirect_offset, uint32_t max_draw_count,
                         uint64_t shader_key);

    /// Get encoded commands for submission
    const std::vector<DrawCommand>& commands() const { return commands_; }

    /// Clear commands for next frame
    void reset() { commands_.clear(); draw_call_count_ = 0; triangle_count_ = 0; }

    /// Statistics
    uint32_t draw_call_count() const { return draw_call_count_; }
    uint64_t triangle_count() const { return triangle_count_; }

private:
    std::vector<DrawCommand> commands_;
    uint32_t draw_call_count_ = 0;
    uint64_t triangle_count_  = 0;
};

} // namespace pictor
