// Null backend (PICTOR_RHI_NULL): records, draws nothing
// (spec/architecture/multi-rhi.md §6 S0).

#include "rhi/null/null_device.h"
#include "test_common.h"

#include <cstring>

using namespace pictor_test;
using namespace pictor::gfx;
using pictor::rhi::NullDevice;
using pictor::rhi::NullFrameStats;
using pictor::rhi::NullSubmitStatus;

static void test_report_and_caps() {
    NullDevice dev;
    const BackendReport& rep = dev.report();
    PT_ASSERT(rep.has_selection, "null reports a selection");
    PT_ASSERT(rep.selected == BackendKind::Null, "selected null");
    PT_ASSERT_OP(rep.attempts.size(), ==, 1u, "one attempt");
    PT_ASSERT(rep.attempts[0].outcome == AttemptOutcome::Selected, "attempt selected");
    PT_ASSERT(rep.degradations.empty(), "no degradations");
    PT_ASSERT(std::strcmp(backend_kind_name(rep.selected), "null") == 0, "kind name");
    PT_ASSERT(dev.caps().tier == CapabilityTier::C_Full, "default tier C");
    PT_ASSERT(dev.caps().features.has(Feature::Compute), "default has compute");
}

static void test_resource_lifetime() {
    NullDevice dev;
    auto& res = dev.resources();
    BufferDesc bd;
    bd.size = 4096;
    bd.usage = BufferUsageVertex | BufferUsageCopyDst;
    bd.debug_name = "vb";
    const BufferId buf = res.create_buffer(bd);
    PT_ASSERT(buf.valid(), "buffer handle valid");
    const BufferDesc* kept = res.find(buf);
    PT_ASSERT(kept != nullptr, "desc retained");
    if (kept) {
        PT_ASSERT(kept->size == 4096, "size retained");
        PT_ASSERT(kept->debug_name == nullptr, "debug name not retained");
    }

    TextureDesc td;
    td.format = GfxFormat::D32Float;
    td.usage = TextureUsageDepthTarget;
    const TextureId tex = res.create_texture(td);
    PipelineDesc pd;
    pd.shader = ShaderBundleRef{"pictor/test_triangle"};
    const PipelineId pipe = res.create_pipeline(pd);
    PT_ASSERT(res.find(pipe) && res.find(pipe)->shader.name == "pictor/test_triangle", "pipeline desc");
    PT_ASSERT(res.find(tex) && is_depth_format(res.find(tex)->format), "texture desc");
    PT_ASSERT_OP(res.live_count(), ==, 3u, "three live");

    PT_ASSERT(res.destroy(buf), "destroy buffer");
    PT_ASSERT(!res.alive(buf) && res.find(buf) == nullptr, "destroyed buffer is stale");
    PT_ASSERT(!res.destroy(buf), "double destroy rejected");
    const BufferId again = res.create_buffer(bd);
    PT_ASSERT(again != buf, "slot reuse issues a new generation");
    PT_ASSERT(!res.alive(buf), "old handle still stale");
}

static void test_frame_records_passes() {
    NullDevice dev;
    auto& res = dev.resources();
    const PipelineId  pipe = res.create_pipeline(PipelineDesc{});
    const BindGroupId bg   = res.create_bind_group();
    const BufferId    vb   = res.create_buffer(BufferDesc{1024, BufferUsageVertex});
    const RenderTargetSetId rts = res.create_render_target_set();

    PassRecorder r = dev.make_recorder();
    PT_ASSERT(dev.submit(r, PassContext{}) == NullSubmitStatus::RejectedOutsideFrame,
              "submit before begin_frame rejected");

    dev.begin_frame();
    r.set_pipeline(pipe);
    r.set_bind_group(0, bg);
    r.set_vertex_buffer(0, vb);
    r.draw(3);
    r.draw(6, 2);
    PassContext ctx;
    ctx.pass_index = 0;
    ctx.width = 320;
    ctx.height = 200;
    ctx.target = rts;
    PT_ASSERT(dev.submit(r, ctx) == NullSubmitStatus::Accepted, "first pass accepted");

    PassRecorder c = dev.make_recorder();
    c.dispatch(4, 4, 1);
    ctx.pass_index = 1;
    PT_ASSERT(dev.submit(c, ctx) == NullSubmitStatus::Accepted, "compute pass accepted");

    const NullFrameStats s = dev.end_frame();
    PT_ASSERT_OP(s.frame_index, ==, 0u, "frame 0");
    PT_ASSERT_OP(s.pass_count, ==, 2u, "two passes");
    PT_ASSERT_OP(s.command_count, ==, 6u, "six commands");
    PT_ASSERT_OP(s.draw_count, ==, 2u, "two draws");
    PT_ASSERT_OP(s.dispatch_count, ==, 1u, "one dispatch");
    PT_ASSERT_OP(s.stale_handle_count, ==, 0u, "no stale handles");

    const auto& log = dev.log();
    PT_ASSERT_OP(log.passes().size(), ==, 2u, "log keeps passes after end_frame");
    if (log.passes().size() == 2) {
        PT_ASSERT_OP(log.passes()[0].command_count, ==, 5u, "pass 0 range");
        PT_ASSERT_OP(log.passes()[1].first_command, ==, 5u, "pass 1 starts after pass 0");
        PT_ASSERT(log.passes()[0].context.target == rts, "context kept");
        PT_ASSERT_OP(log.passes()[0].context.width, ==, 320u, "context extent kept");
    }
    if (log.commands().size() == 6) {
        PT_ASSERT(log.commands()[5].op == PassOp::Dispatch, "flat array in submission order");
    }

    dev.begin_frame();
    PT_ASSERT(dev.log().passes().empty(), "next frame starts empty");
    PT_ASSERT_OP(dev.end_frame().frame_index, ==, 1u, "frame index advances");
}

static void test_stale_handles_and_rejections() {
    NullDevice dev;
    auto& res = dev.resources();
    const BufferId vb = res.create_buffer(BufferDesc{256, BufferUsageVertex});
    res.destroy(vb);

    dev.begin_frame();
    PassRecorder r = dev.make_recorder();
    r.set_vertex_buffer(0, vb);   // handle is well-formed but its resource is gone
    r.draw(3);
    PT_ASSERT(dev.submit(r, PassContext{}) == NullSubmitStatus::AcceptedWithStaleHandles,
              "stale handle reported");

    PassRecorder bad = dev.make_recorder();
    bad.set_pipeline(PipelineId{});
    PT_ASSERT(dev.submit(bad, PassContext{}) == NullSubmitStatus::RejectedRecorderErrors,
              "recorder errors reject the pass");
    const NullFrameStats s = dev.end_frame();
    PT_ASSERT_OP(s.stale_handle_count, ==, 1u, "one stale reference");
    PT_ASSERT_OP(s.rejected_passes, ==, 1u, "one rejected pass");
    PT_ASSERT_OP(s.pass_count, ==, 1u, "rejected pass not recorded");
}

static void test_raster_only_device() {
    DeviceCaps caps;
    caps.tier = CapabilityTier::A_Raster;   // WebGL2 class: no compute
    NullDevice dev(caps);
    PT_ASSERT(dev.report().tier == CapabilityTier::A_Raster, "report carries tier");
    PassRecorder r = dev.make_recorder();
    PT_ASSERT(!r.compute_available(), "recorder follows device compute feature");
    r.dispatch(1);
    dev.begin_frame();
    PT_ASSERT(dev.submit(r, PassContext{}) == NullSubmitStatus::RejectedRecorderErrors,
              "dispatch on tier A is an explicit error");
    dev.end_frame();
}

int main() {
    test_report_and_caps();
    test_resource_lifetime();
    test_frame_records_passes();
    test_stale_handles_and_rejections();
    test_raster_only_device();
    return report("unit_null_backend_test");
}
