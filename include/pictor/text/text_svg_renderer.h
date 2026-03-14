#pragma once

#include "pictor/text/text_types.h"
#include "pictor/text/font_loader.h"
#include <string>
#include <vector>

namespace pictor {

/// SVG text renderer.
///
/// Extracts glyph outlines from TrueType fonts as vector path data
/// and produces SVG-compatible output. Two rendering modes:
///
/// 1. **Glyph outline extraction**: Parse 'glyf' table contours into
///    SvgPathPoint sequences (quadratic Bezier curves → SVG path data).
///
/// 2. **Full SVG document generation**: Lay out text strings and emit
///    complete SVG documents with <path> elements for each glyph.
///
/// Use cases:
///   - Resolution-independent text rendering
///   - Export text as scalable vector graphics
///   - Debug visualization of glyph outlines
///   - Pre-processing step for GPU-based vector text rendering
class TextSvgRenderer {
public:
    explicit TextSvgRenderer(const FontLoader& font_loader);
    ~TextSvgRenderer() = default;

    TextSvgRenderer(const TextSvgRenderer&) = delete;
    TextSvgRenderer& operator=(const TextSvgRenderer&) = delete;

    // ---- Glyph outline extraction ----

    /// Extract the outline of a single glyph as path commands.
    /// Returns empty outline if the glyph is not found or has no contours.
    GlyphOutline extract_glyph_outline(FontHandle font, uint32_t codepoint) const;

    /// Extract outlines for all glyphs in the given character set.
    std::vector<GlyphOutline> extract_charset_outlines(FontHandle font,
                                                        CharSet charset) const;

    // ---- SVG path string generation ----

    /// Convert a GlyphOutline to an SVG path 'd' attribute string.
    /// Coordinates are in font units unless scale is applied.
    static std::string outline_to_svg_path(const GlyphOutline& outline,
                                            float scale = 1.0f,
                                            float offset_x = 0.0f,
                                            float offset_y = 0.0f);

    // ---- Full SVG document generation ----

    /// Render a text string as a complete SVG document.
    /// Returns the SVG XML string.
    std::string render_text_svg(FontHandle font, const std::string& text,
                                const TextStyle& style = {}) const;

    /// Render a single glyph as a standalone SVG document.
    std::string render_glyph_svg(FontHandle font, uint32_t codepoint,
                                  float size = 64.0f) const;

    /// SVG generation options
    struct SvgOptions {
        bool   include_xml_header = true;   // <?xml ...?>
        bool   include_background = false;
        float4 background_color   = {0.0f, 0.0f, 0.0f, 0.0f};
        bool   flip_y             = true;   // Font coords are Y-up, SVG is Y-down
        float  padding            = 4.0f;   // Padding around text in pixels
        std::string css_class;              // Optional CSS class for <svg> element
    };

    /// Render text with custom SVG options.
    std::string render_text_svg(FontHandle font, const std::string& text,
                                const TextStyle& style,
                                const SvgOptions& options) const;

private:
    /// Parse TrueType 'glyf' table contours for a glyph index.
    GlyphOutline parse_truetype_glyph(const FontTableEntry& entry,
                                       uint16_t glyph_index,
                                       uint32_t codepoint) const;

    /// Decode UTF-8 string to codepoints
    static std::vector<uint32_t> utf8_to_codepoints(const std::string& text);

    /// Format a float for SVG (minimal decimal places)
    static std::string fmt(float v);

    /// Format color as SVG hex string
    static std::string color_to_hex(const float4& color);

    const FontLoader& font_loader_;
};

} // namespace pictor
