#pragma once

#include <cstdint>

namespace pictor {

/// Backend-independent outcome. A lost device requires explicit reinitialization;
/// it must never be interpreted as an ordinary window resize.
/// Recovery order: spec/feature/portability/mobile-surface-recovery.md
enum class FrameStatus : uint8_t {
    Ready,
    RecreateSwapchain,
    SurfaceLost,
    DeviceLost,
    NotInitialized,
    Error,
    /// The host suspended presentation (pause / suspend / surface lost).
    /// No native acquire, submit or present was issued for this frame.
    Suspended,
};

struct FrameResult {
    FrameStatus status = FrameStatus::NotInitialized;
    uint32_t image_index = UINT32_MAX;
    /// A suboptimal acquire still owns an image: submit/present it before resize.
    bool recreate_requested = false;
    /// The context rebuilt its swapchain during this call. Host-owned resources
    /// that depend on swapchain images or extent must be rebuilt before reuse.
    bool swapchain_recreated = false;

    bool has_image() const {
        return status == FrameStatus::Ready && image_index != UINT32_MAX;
    }
};

/// True for outcomes that only an explicit teardown + reinitialize can clear.
inline bool frame_status_requires_reinitialize(FrameStatus status) {
    return status == FrameStatus::SurfaceLost ||
           status == FrameStatus::DeviceLost ||
           status == FrameStatus::Error;
}

} // namespace pictor
