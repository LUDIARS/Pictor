#include "rhi/null/null_device.h"

namespace pictor::rhi {

gfx::DeviceCaps NullDevice::default_caps() {
    gfx::DeviceCaps caps;
    caps.tier = gfx::CapabilityTier::C_Full;
    for (uint32_t f = 0; f < static_cast<uint32_t>(gfx::Feature::Count_); ++f) {
        caps.features.set(static_cast<gfx::Feature>(f));
    }
    return caps;
}

NullDevice::NullDevice(const gfx::DeviceCaps& caps) : caps_(caps) {
    report_.has_selection = true;
    report_.selected      = gfx::BackendKind::Null;
    report_.tier          = caps_.tier;
    report_.attempts.push_back(gfx::BackendAttempt{
        gfx::BackendKind::Null, gfx::AttemptOutcome::Selected, "headless: records only, draws nothing"});
}

void NullDevice::begin_frame() {
    log_.clear();
    rejected_passes_ = 0;
    in_frame_ = true;
}

NullSubmitStatus NullDevice::submit(const gfx::PassRecorder& recorder,
                                    const gfx::PassContext& context) {
    if (!in_frame_) return NullSubmitStatus::RejectedOutsideFrame;
    if (!recorder.ok()) {
        ++rejected_passes_;
        return NullSubmitStatus::RejectedRecorderErrors;
    }
    const NullRecordedPass& pass = log_.append(recorder, context, resources_);
    return pass.stale_handle_count == 0 ? NullSubmitStatus::Accepted
                                        : NullSubmitStatus::AcceptedWithStaleHandles;
}

NullFrameStats NullDevice::end_frame() {
    NullFrameStats stats;
    stats.frame_index     = frame_index_;
    stats.pass_count      = static_cast<uint32_t>(log_.passes().size());
    stats.command_count   = static_cast<uint32_t>(log_.commands().size());
    stats.rejected_passes = rejected_passes_;
    for (const NullRecordedPass& p : log_.passes()) stats.stale_handle_count += p.stale_handle_count;
    for (const gfx::PassCommand& c : log_.commands()) {
        if (c.op == gfx::PassOp::Draw || c.op == gfx::PassOp::DrawIndexed) ++stats.draw_count;
        else if (c.op == gfx::PassOp::Dispatch) ++stats.dispatch_count;
    }
    in_frame_ = false;
    ++frame_index_;
    return stats;
}

} // namespace pictor::rhi
