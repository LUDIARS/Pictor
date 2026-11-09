#pragma once
#include "pictor/text/font_loader.h"

namespace pictor::detail {
// Font-unit cubic paths from an OpenType font's 'CFF ' table (Type 2
// charstrings, name-keyed or CID-keyed). Throws std::invalid_argument on
// malformed data. No hinting is applied.
[[nodiscard]] bool has_cff_outlines(const FontTableEntry& font);
GlyphOutline decode_cff_outline(const FontTableEntry& font, uint16_t glyph, uint32_t codepoint);
}  // namespace pictor::detail
