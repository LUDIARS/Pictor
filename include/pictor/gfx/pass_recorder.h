#pragma once

#include "pictor/gfx/format.h"
#include "pictor/gfx/handle.h"
#include "pictor/gfx/pass_command.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pictor::gfx {

/// Why a record call was rejected. Rejected calls append nothing and are
/// counted, so a pass that tried something the device cannot do fails
/// loudly instead of rendering partially (multi-rhi.md §5 "独自パス").
enum class RecordError : uint8_t {
    Ok = 0,
    InvalidHandle,          ///< default-constructed / never-issued handle
    BindGroupOutOfRange,    ///< group index >= kMaxBindGroups
    ComputeUnavailable,     ///< Dispatch on a device without compute (Tier A)
};

struct Viewport {
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
    float min_depth = 0.0f, max_depth = 1.0f;
};

struct ScissorRect {
    int32_t  x = 0, y = 0;
    uint32_t width = 0, height = 0;
};

/// Records a custom pass as a flat array of POD PassCommand values.
///
/// It only appends: no virtual calls, no backend calls, no allocation once the
/// array has grown to the pass's size (call reserve() or reuse the recorder
/// with clear()). A backend replays commands() later; the null backend just
/// keeps them, which is what makes recorded passes testable headless.
class PassRecorder final {
public:
    static constexpr uint32_t kMaxBindGroups = 4;  ///< FRAME / PASS / MATERIAL / DRAW

    explicit PassRecorder(bool compute_available = true) noexcept
        : compute_available_(compute_available) {}

    void reserve(size_t command_count) { commands_.reserve(command_count); }

    /// Drop recorded commands and errors; keeps capacity.
    void clear() noexcept {
        commands_.clear();
        first_error_ = RecordError::Ok;
        error_count_ = 0;
    }

    void set_pipeline(PipelineId pipeline) {
        if (!pipeline.valid()) { fail(RecordError::InvalidHandle); return; }
        PassCommand& c = append(PassOp::SetPipeline);
        c.handle = pipeline.bits;
    }

    void set_bind_group(uint32_t group, BindGroupId bind_group, uint32_t dynamic_offset = 0) {
        if (group >= kMaxBindGroups) { fail(RecordError::BindGroupOutOfRange); return; }
        if (!bind_group.valid())     { fail(RecordError::InvalidHandle); return; }
        PassCommand& c = append(PassOp::SetBindGroup);
        c.slot    = static_cast<uint8_t>(group);
        c.handle  = bind_group.bits;
        c.args[0] = dynamic_offset;
    }

    void set_vertex_buffer(uint32_t binding, BufferId buffer, uint32_t offset = 0) {
        if (!buffer.valid()) { fail(RecordError::InvalidHandle); return; }
        PassCommand& c = append(PassOp::SetVertexBuffer);
        c.slot    = static_cast<uint8_t>(binding);
        c.handle  = buffer.bits;
        c.args[0] = offset;
    }

    void set_index_buffer(BufferId buffer, IndexFormat format, uint32_t offset = 0) {
        if (!buffer.valid()) { fail(RecordError::InvalidHandle); return; }
        PassCommand& c = append(PassOp::SetIndexBuffer);
        c.aux     = static_cast<uint8_t>(format);
        c.handle  = buffer.bits;
        c.args[0] = offset;
    }

    void set_viewport(const Viewport& v) {
        PassCommand& c = append(PassOp::SetViewport);
        c.args[0] = pack_float(v.x);
        c.args[1] = pack_float(v.y);
        c.args[2] = pack_float(v.width);
        c.args[3] = pack_float(v.height);
        c.args[4] = pack_float(v.min_depth);
        c.args[5] = pack_float(v.max_depth);
    }

    void set_scissor(const ScissorRect& r) {
        PassCommand& c = append(PassOp::SetScissor);
        c.args[0] = pack_int(r.x);
        c.args[1] = pack_int(r.y);
        c.args[2] = r.width;
        c.args[3] = r.height;
    }

    void draw(uint32_t vertex_count, uint32_t instance_count = 1,
              uint32_t first_vertex = 0, uint32_t first_instance = 0) {
        PassCommand& c = append(PassOp::Draw);
        c.args[0] = vertex_count;
        c.args[1] = instance_count;
        c.args[2] = first_vertex;
        c.args[3] = first_instance;
    }

    void draw_indexed(uint32_t index_count, uint32_t instance_count = 1,
                      uint32_t first_index = 0, int32_t vertex_offset = 0,
                      uint32_t first_instance = 0) {
        PassCommand& c = append(PassOp::DrawIndexed);
        c.args[0] = index_count;
        c.args[1] = instance_count;
        c.args[2] = first_index;
        c.args[3] = pack_int(vertex_offset);
        c.args[4] = first_instance;
    }

    void dispatch(uint32_t group_count_x, uint32_t group_count_y = 1, uint32_t group_count_z = 1) {
        if (!compute_available_) { fail(RecordError::ComputeUnavailable); return; }
        PassCommand& c = append(PassOp::Dispatch);
        c.args[0] = group_count_x;
        c.args[1] = group_count_y;
        c.args[2] = group_count_z;
    }

    const PassCommand* data() const noexcept { return commands_.data(); }
    size_t             size() const noexcept { return commands_.size(); }
    bool               empty() const noexcept { return commands_.empty(); }
    const std::vector<PassCommand>& commands() const noexcept { return commands_; }

    bool        ok() const noexcept { return error_count_ == 0; }
    RecordError first_error() const noexcept { return first_error_; }
    uint32_t    error_count() const noexcept { return error_count_; }
    bool        compute_available() const noexcept { return compute_available_; }

private:
    PassCommand& append(PassOp op) {
        PassCommand& c = commands_.emplace_back();
        c.op = op;
        return c;
    }

    void fail(RecordError e) noexcept {
        if (first_error_ == RecordError::Ok) first_error_ = e;
        ++error_count_;
    }

    std::vector<PassCommand> commands_;
    RecordError              first_error_       = RecordError::Ok;
    uint32_t                 error_count_       = 0;
    bool                     compute_available_ = true;
};

const char* record_error_name(RecordError e) noexcept;

} // namespace pictor::gfx
