#pragma once
#include "pictor/text/font_loader.h"

namespace pictor::detail {
// Font-unit paths; no pixel size or platform state enters outline decoding.
GlyphOutline decode_truetype_outline(const FontTableEntry& font,
                                    uint16_t glyph, uint32_t codepoint);
}
