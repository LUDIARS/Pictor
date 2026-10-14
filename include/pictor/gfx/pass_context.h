#pragma once

#include "pictor/gfx/handle.h"

#include <cstdint>

namespace pictor::gfx {

/// Read-only facts handed to a custom pass callback
/// `void(PassRecorder&, const PassContext&)`. Deliberately carries no backend
/// identity or capability flags: a pass records the same commands everywhere,
/// and capability gaps are resolved by declared downgrades (multi-rhi.md §2).
struct PassContext {
    uint32_t          frame_index = 0;   ///< monotonically increasing frame counter
    uint32_t          pass_index  = 0;   ///< position of the pass in the compiled graph
    uint32_t          width       = 0;   ///< render area in pixels
    uint32_t          height      = 0;
    RenderTargetSetId target      = {};  ///< attachments the pass renders into
};

} // namespace pictor::gfx
