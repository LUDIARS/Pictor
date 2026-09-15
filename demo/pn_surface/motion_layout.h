#pragma once
#include "pictor/demo/polynomial_motion.h"
#include <cstdint>
#include <span>
#include <vector>

namespace pictor_pn {
/// One raymarch draw call: a contiguous patch range bound to one texture set.
struct PatchDrawRange { uint32_t submesh=0,first_patch=0,count=0; };

// Upper bound on appended patches. 2^20 * 272 bytes keeps the persistently
// mapped storage buffer below 300 MiB for a second-level subdivided character.
inline constexpr uint32_t kMaxAppendedPatches=1u<<20;
// Draw calls are recorded per frame; bound them so a scattered provider layout
// fails at initialization instead of degrading every frame.
inline constexpr size_t kMaxDrawRanges=4096;

/// Validates a provider patch layout once and returns its fixed draw ranges.
/// Source ranges come first unless the provider replaces the source surface.
/// Untextured runs bind submesh 0, matching the constant-color shader path.
/// @implements SPEC-PC-POLYNOMIAL-MOTION
std::vector<PatchDrawRange> motion_draw_ranges(std::span<const pictor::demo::MotionSubmesh> submeshes,
    std::span<const pictor::demo::PolynomialPatch> patches,uint32_t source_count,bool replaces_source);
}
