#pragma once

#include "pictor/text/text_types.h"

#include <cstdint>

namespace pictor {

class FontLoader;

/// Where glyph outlines and spacing come from. The cache and the layout depend
/// on this, not on a font file format.
class IGlyphOutlineSource {
public:
    virtual ~IGlyphOutlineSource() = default;
    /// Outline in font units (y-up). An empty path is a blank glyph.
    virtual GlyphOutline outline(FontHandle font, uint32_t codepoint) const = 0;
    /// Font units per em.
    virtual float units_per_em(FontHandle font) const = 0;
    /// Kerning between two codepoints, in font units.
    virtual float kerning(FontHandle font, uint32_t left, uint32_t right) const = 0;
    /// Baseline-to-baseline distance in font units.
    virtual float line_height(FontHandle font) const = 0;
};

/// Reads TrueType outlines through FontLoader (borrowed; must outlive this).
class FontOutlineSource final : public IGlyphOutlineSource {
public:
    explicit FontOutlineSource(const FontLoader& fonts) : fonts_(fonts) {}
    GlyphOutline outline(FontHandle font, uint32_t codepoint) const override;
    float units_per_em(FontHandle font) const override;
    float kerning(FontHandle font, uint32_t left, uint32_t right) const override;
    float line_height(FontHandle font) const override;

private:
    const FontLoader& fonts_;
};

} // namespace pictor
