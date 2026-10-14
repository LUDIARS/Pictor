#pragma once

#include <cstdint>

namespace pictor::gfx {

/// What a pass does with an attachment's previous contents.
enum class LoadOp : uint8_t {
    Load = 0,
    Clear,
    DontCare,
};

/// Whether a pass keeps what it rendered into an attachment.
enum class StoreOp : uint8_t {
    Store = 0,
    DontCare,
};

} // namespace pictor::gfx
