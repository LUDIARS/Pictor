#pragma once

#include <cstdint>

namespace pictor {

/// Glyph meshes are built once per size tier. Tier t covers pixel sizes up to
/// 8 * 2^t, so a glyph drawn anywhere in its tier is flattened at least as
/// finely as `kTierTolerancePx`.
inline constexpr uint32_t kGlyphSizeTierCount = 10;  // up to 4096 px per em
inline constexpr float    kTierTolerancePx    = 0.2f;

/// Tier for an em size in pixels (larger sizes use the last tier).
uint32_t glyph_size_tier(float pixels_per_em);

/// Flattening tolerance in em units for a tier.
float glyph_tier_tolerance_em(uint32_t tier);

} // namespace pictor
