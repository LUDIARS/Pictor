#pragma once
#include "pictor/text/font_loader.h"

namespace pictor::detail {
ImageBuffer rasterize_font_glyph(const FontLoader& fonts, FontHandle font,
                                uint32_t codepoint, float size, GlyphMetrics& metrics);
}
