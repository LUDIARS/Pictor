#include "pictor/vector_text/size_tier.h"

namespace pictor {

uint32_t glyph_size_tier(float pixels_per_em) {
    float top = 8.0f;
    for (uint32_t tier = 0; tier + 1 < kGlyphSizeTierCount; ++tier) {
        if (pixels_per_em <= top) return tier;  // also catches NaN-free tiny or negative sizes
        top *= 2.0f;
    }
    return kGlyphSizeTierCount - 1;
}

float glyph_tier_tolerance_em(uint32_t tier) {
    if (tier >= kGlyphSizeTierCount) tier = kGlyphSizeTierCount - 1;
    const float top = 8.0f * static_cast<float>(1U << tier);
    return kTierTolerancePx / top;
}

}  // namespace pictor
