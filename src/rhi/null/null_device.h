#pragma once

#include "pictor/gfx/backend_report.h"
#include "pictor/gfx/device_caps.h"
#include "pictor/gfx/pass_context.h"
#include "pictor/gfx/pass_recorder.h"
#include "rhi/null/null_pass_log.h"
#include "rhi/null/null_resource_table.h"

#include <cstdint>

namespace pictor::rhi {

/// Outcome of NullDevice::submit.
enum class NullSubmitStatus : uint8_t {
    Accepted = 0,
    AcceptedWithStaleHandles,  ///< recorded, but some command names a dead resource
    RejectedRecorderErrors,    ///< recorder reported errors; nothing recorded
    RejectedOutsideFrame,      ///< submit without begin_frame
};

/// Totals of one frame, returned by NullDevice::end_frame.
struct NullFrameStats {
    uint32_t frame_index        = 0;
    uint32_t pass_count         = 0;
    uint32_t command_count      = 0;
    uint32_t draw_count         = 0;   ///< Draw + DrawIndexed
    uint32_t dispatch_count     = 0;
    uint32_t stale_handle_count = 0;
    uint32_t rejected_passes    = 0;
};

/// The null backend (PICTOR_RHI_NULL): records, draws nothing.
///
/// Issues real generation-tagged handles, keeps descriptors, and stores every
/// submitted pass's commands, so headless tests can check what a frame would
/// have rendered on any backend. Its capability table is configurable so the
/// same tests can stand in for a Tier A / B / C device.
class NullDevice final {
public:
    static gfx::DeviceCaps default_caps();   ///< Tier C, every feature

    explicit NullDevice(const gfx::DeviceCaps& caps = default_caps());

    const gfx::DeviceCaps&   caps() const noexcept { return caps_; }
    const gfx::BackendReport& report() const noexcept { return report_; }

    NullResourceTable&       resources() noexcept { return resources_; }
    const NullResourceTable& resources() const noexcept { return resources_; }

    /// Recorder whose Dispatch acceptance matches this device's Compute feature.
    gfx::PassRecorder make_recorder() const {
        return gfx::PassRecorder(caps_.features.has(gfx::Feature::Compute));
    }

    void             begin_frame();
    NullSubmitStatus submit(const gfx::PassRecorder& recorder, const gfx::PassContext& context);
    NullFrameStats   end_frame();

    bool               in_frame() const noexcept { return in_frame_; }
    uint32_t           frame_index() const noexcept { return frame_index_; }
    /// Passes of the current frame, or of the last ended one until the next begin_frame.
    const NullPassLog& log() const noexcept { return log_; }

private:
    gfx::DeviceCaps    caps_;
    gfx::BackendReport report_;
    NullResourceTable  resources_;
    NullPassLog        log_;
    uint32_t           frame_index_     = 0;
    uint32_t           rejected_passes_ = 0;
    bool               in_frame_        = false;
};

} // namespace pictor::rhi
