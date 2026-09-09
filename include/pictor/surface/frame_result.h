#pragma once

#include <cstdint>

namespace pictor {

/// Backend-independent outcome. A lost device requires explicit reinitialization;
/// it must never be interpreted as an ordinary window resize.
enum class FrameStatus : uint8_t {
    Ready,
    RecreateSwapchain,
    SurfaceLost,
    DeviceLost,
    NotInitialized,
    Error,
};

struct FrameResult {
    FrameStatus status = FrameStatus::NotInitialized;
    uint32_t image_index = UINT32_MAX;
    /// A suboptimal acquire still owns an image: submit/present it before resize.
    bool recreate_requested = false;

    bool has_image() const {
        return status == FrameStatus::Ready && image_index != UINT32_MAX;
    }
};

} // namespace pictor
