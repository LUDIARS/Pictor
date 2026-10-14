#pragma once

#include <cstdint>

namespace pictor::gfx {

/// Depth / sampler comparison.
///
/// The two constant results are spelled `NeverPass` / `AlwaysPass` rather
/// than `Never` / `Always`: X11's <X11/X.h> (pulled in by some platform
/// window headers) defines `Always` as a macro, which would break any
/// translation unit that includes it first.
enum class CompareOp : uint8_t {
    NeverPass = 0,
    Less,
    Equal,
    LessOrEqual,
    Greater,
    NotEqual,
    GreaterOrEqual,
    AlwaysPass,
};

} // namespace pictor::gfx
