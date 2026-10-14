#pragma once

#include <bit>
#include <cstdint>

namespace pictor::gfx {

/// Opcode of one recorded command.
enum class PassOp : uint8_t {
    SetPipeline = 0,
    SetBindGroup,
    SetVertexBuffer,
    SetIndexBuffer,
    SetViewport,
    SetScissor,
    Draw,
    DrawIndexed,
    Dispatch,
    Count_
};

/// One backend-neutral command: plain old data, fixed 32 bytes, two per
/// 64-byte cache line. A PassRecorder appends these into one flat array; each
/// backend replays the array with a direct loop (no virtual call per command).
///
/// Field meaning per op (args are raw 32-bit words; floats / signed values are
/// stored with std::bit_cast, read them back with arg_float / arg_int):
///
///   SetPipeline     handle = PipelineId
///   SetBindGroup    slot = group (0..3), handle = BindGroupId, args[0] = dynamic offset
///   SetVertexBuffer slot = binding,      handle = BufferId,    args[0] = byte offset
///   SetIndexBuffer  aux  = IndexFormat,  handle = BufferId,    args[0] = byte offset
///   SetViewport     args[0..5] = x, y, width, height, min_depth, max_depth (float)
///   SetScissor      args[0..3] = x, y (int), width, height
///   Draw            args[0..3] = vertex_count, instance_count, first_vertex, first_instance
///   DrawIndexed     args[0..4] = index_count, instance_count, first_index,
///                                vertex_offset (int), first_instance
///   Dispatch        args[0..2] = group_count_x, y, z
struct alignas(16) PassCommand {
    static constexpr uint32_t kArgCount = 6;

    PassOp   op       = PassOp::Draw;
    uint8_t  slot     = 0;
    uint8_t  aux      = 0;
    uint8_t  reserved = 0;
    uint32_t handle   = 0;          ///< raw handle bits (0 = none)
    uint32_t args[kArgCount] = {};
};
static_assert(sizeof(PassCommand) == 32, "PassCommand is a fixed 32-byte record");

constexpr uint32_t pack_float(float v) noexcept { return std::bit_cast<uint32_t>(v); }
constexpr uint32_t pack_int(int32_t v) noexcept { return std::bit_cast<uint32_t>(v); }
constexpr float    arg_float(const PassCommand& c, uint32_t i) noexcept { return std::bit_cast<float>(c.args[i]); }
constexpr int32_t  arg_int(const PassCommand& c, uint32_t i) noexcept { return std::bit_cast<int32_t>(c.args[i]); }

} // namespace pictor::gfx
