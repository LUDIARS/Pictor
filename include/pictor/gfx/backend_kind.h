#pragma once

#include <cstdint>

namespace pictor::gfx {

/// Which rendering backend a RenderSystem ended up on. Read-only for
/// consumers: it is shown in BackendReport / logs, never used to branch
/// rendering code (spec/architecture/multi-rhi.md §1, §2).
enum class BackendKind : uint8_t {
    Null = 0,   ///< records only, draws nothing (headless tests)
    Vulkan,
    D3D12,
    Metal,
    WebGPU,
    WebGL2,
    Count_
};

/// Stable lower-case label ("null", "vulkan", ...). Returns "unknown" for
/// out-of-range values.
const char* backend_kind_name(BackendKind kind) noexcept;

} // namespace pictor::gfx
