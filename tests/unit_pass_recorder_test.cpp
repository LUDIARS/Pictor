// PassRecorder records backend-neutral POD commands into one flat array
// (spec/architecture/multi-rhi.md §5 独自パス, §6 S0).

#include "pictor/gfx/pass_recorder.h"
#include "test_common.h"

#include <cstring>
#include <type_traits>

using namespace pictor_test;
using namespace pictor::gfx;

static void test_command_is_pod() {
    static_assert(std::is_trivially_copyable_v<PassCommand>);
    static_assert(std::is_standard_layout_v<PassCommand>);
    static_assert(sizeof(PassCommand) == 32);
    static_assert(alignof(PassCommand) == 16);
    static_assert(!std::is_polymorphic_v<PassRecorder>, "recorder has no virtual calls");
}

static void test_records_in_order() {
    const PipelineId  pipe = PipelineId::make(2, 1);
    const BindGroupId bg   = BindGroupId::make(5, 3);
    const BufferId    vb   = BufferId::make(7, 1);
    const BufferId    ib   = BufferId::make(8, 2);

    PassRecorder r;
    r.set_pipeline(pipe);
    r.set_bind_group(3, bg, 256);
    r.set_vertex_buffer(1, vb, 64);
    r.set_index_buffer(ib, IndexFormat::Uint16, 12);
    r.set_viewport(Viewport{0.0f, 0.0f, 1280.0f, 720.0f, 0.0f, 1.0f});
    r.set_scissor(ScissorRect{-4, 8, 640, 360});
    r.draw(3);
    r.draw_indexed(36, 10, 6, -2, 4);
    r.dispatch(8, 4, 2);

    PT_ASSERT(r.ok(), "no errors");
    PT_ASSERT_OP(r.size(), ==, 9u, "nine commands");
    if (r.size() != 9) return;
    const PassCommand* c = r.data();

    PT_ASSERT(c[0].op == PassOp::SetPipeline, "op 0");
    PT_ASSERT_OP(c[0].handle, ==, pipe.bits, "pipeline handle");

    PT_ASSERT(c[1].op == PassOp::SetBindGroup, "op 1");
    PT_ASSERT_OP(c[1].slot, ==, 3, "bind group index");
    PT_ASSERT_OP(c[1].handle, ==, bg.bits, "bind group handle");
    PT_ASSERT_OP(c[1].args[0], ==, 256u, "dynamic offset");

    PT_ASSERT(c[2].op == PassOp::SetVertexBuffer, "op 2");
    PT_ASSERT_OP(c[2].slot, ==, 1, "vertex binding");
    PT_ASSERT_OP(c[2].handle, ==, vb.bits, "vertex buffer");
    PT_ASSERT_OP(c[2].args[0], ==, 64u, "vertex offset");

    PT_ASSERT(c[3].op == PassOp::SetIndexBuffer, "op 3");
    PT_ASSERT_OP(c[3].aux, ==, static_cast<uint8_t>(IndexFormat::Uint16), "index format");
    PT_ASSERT_OP(c[3].handle, ==, ib.bits, "index buffer");
    PT_ASSERT_OP(c[3].args[0], ==, 12u, "index offset");

    PT_ASSERT(c[4].op == PassOp::SetViewport, "op 4");
    PT_ASSERT(arg_float(c[4], 2) == 1280.0f && arg_float(c[4], 3) == 720.0f, "viewport size");
    PT_ASSERT(arg_float(c[4], 5) == 1.0f, "viewport max depth");

    PT_ASSERT(c[5].op == PassOp::SetScissor, "op 5");
    PT_ASSERT_OP(arg_int(c[5], 0), ==, -4, "scissor x keeps sign");
    PT_ASSERT_OP(c[5].args[2], ==, 640u, "scissor width");

    PT_ASSERT(c[6].op == PassOp::Draw, "op 6");
    PT_ASSERT_OP(c[6].args[0], ==, 3u, "vertex count");
    PT_ASSERT_OP(c[6].args[1], ==, 1u, "default instance count");

    PT_ASSERT(c[7].op == PassOp::DrawIndexed, "op 7");
    PT_ASSERT_OP(c[7].args[0], ==, 36u, "index count");
    PT_ASSERT_OP(c[7].args[1], ==, 10u, "instance count");
    PT_ASSERT_OP(c[7].args[2], ==, 6u, "first index");
    PT_ASSERT_OP(arg_int(c[7], 3), ==, -2, "vertex offset keeps sign");
    PT_ASSERT_OP(c[7].args[4], ==, 4u, "first instance");

    PT_ASSERT(c[8].op == PassOp::Dispatch, "op 8");
    PT_ASSERT(c[8].args[0] == 8 && c[8].args[1] == 4 && c[8].args[2] == 2, "group counts");
}

static void test_rejections_are_counted_not_dropped_silently() {
    PassRecorder r;
    r.set_pipeline(PipelineId{});                         // invalid handle
    r.set_bind_group(4, BindGroupId::make(1, 1));         // group out of range
    r.set_vertex_buffer(0, BufferId{});                   // invalid handle
    r.draw(3);
    PT_ASSERT(!r.ok(), "errors reported");
    PT_ASSERT_OP(r.error_count(), ==, 3u, "three rejected calls");
    PT_ASSERT(r.first_error() == RecordError::InvalidHandle, "first error kept");
    PT_ASSERT_OP(r.size(), ==, 1u, "only the valid draw recorded");
}

static void test_dispatch_without_compute() {
    PassRecorder r(/*compute_available=*/false);
    r.draw(6);
    r.dispatch(1);
    PT_ASSERT(!r.ok(), "dispatch on a raster-only device is an error");
    PT_ASSERT(r.first_error() == RecordError::ComputeUnavailable, "compute error");
    PT_ASSERT_OP(r.size(), ==, 1u, "dispatch not appended");
    PT_ASSERT(std::strcmp(record_error_name(r.first_error()), "compute_unavailable") == 0, "error name");
}

static void test_clear_keeps_capacity() {
    PassRecorder r;
    r.reserve(64);
    for (int i = 0; i < 64; ++i) r.draw(3);
    const PassCommand* before = r.data();
    r.set_pipeline(PipelineId{});
    r.clear();
    PT_ASSERT(r.empty() && r.ok(), "clear drops commands and errors");
    for (int i = 0; i < 64; ++i) r.draw(3);
    PT_ASSERT(r.data() == before, "re-recording the same size does not reallocate");
}

int main() {
    test_command_is_pod();
    test_records_in_order();
    test_rejections_are_counted_not_dropped_silently();
    test_dispatch_without_compute();
    test_clear_keeps_capacity();
    return report("unit_pass_recorder_test");
}
