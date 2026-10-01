#pragma once

#include <cstdint>
#include <string>

namespace pictor {

/// Why a surface context did or did not initialize. Capability gaps are
/// reported here instead of being folded into a resize or a skipped frame.
enum class ContextInitStatus : uint8_t {
    Ok,
    NotInitialized,
    /// The host-owned provider exposes no native window / layer (or a zero
    /// extent). Nothing native was created; retry after the surface returns.
    SurfaceUnavailable,
    MissingInstanceExtension,
    MissingDeviceExtension,
    /// A required device feature or surface capability is unsupported
    /// (portability subset feature, multiview, swapchain usage, ...).
    MissingCapability,
    NoSuitableDevice,
    Failed,
};

struct ContextInitResult {
    ContextInitStatus status = ContextInitStatus::NotInitialized;
    /// Name of the missing extension / feature, or a short reason.
    std::string detail;

    bool ok() const { return status == ContextInitStatus::Ok; }
};

} // namespace pictor
