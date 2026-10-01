#pragma once

#include "pictor/surface/frame_result.h"

namespace pictor {

/// State a surface context consults before issuing any native frame call.
struct FrameGateInput {
    bool        initialized = false;
    /// Last recorded outcome; loss / error outcomes are sticky.
    FrameStatus latched = FrameStatus::NotInitialized;
    /// Host-driven lifecycle suppression (pause / suspend / surface lost).
    bool        presentation_suspended = false;
    /// The host-owned provider currently exposes a native window / layer.
    bool        native_surface_available = false;
};

/// Decides whether a frame may touch the native API. Ready means "proceed";
/// any other status is returned to the host unchanged and no native acquire,
/// submit, present or swapchain creation may follow.
///
/// Precedence: NotInitialized > sticky loss/error > Suspended > SurfaceLost.
/// Suspension is checked before the native window so that a host which
/// suspended presentation before releasing the window never latches a loss.
inline FrameResult gate_frame(const FrameGateInput& in) {
    if (!in.initialized) return {FrameStatus::NotInitialized};
    if (frame_status_requires_reinitialize(in.latched)) return {in.latched};
    if (in.presentation_suspended) return {FrameStatus::Suspended};
    if (!in.native_surface_available) return {FrameStatus::SurfaceLost};
    return {FrameStatus::Ready};
}

} // namespace pictor
