#pragma once

#include "pictor/text/text_types.h"
#include "pictor/text/font_loader.h"

namespace pictor {

/// Text-to-image renderer.
///
/// Rasterizes text strings into RGBA ImageBuffer bitmaps that can be
/// uploaded to the GPU as textures via TextureRegistry.
///
/// Pipeline: text string → per-glyph rasterization → compositing → ImageBuffer
///
/// Uses a simple scanline rasterizer for glyph outlines (TrueType quadratic
/// Bezier contours). For CFF/OTF fonts, falls back to the glyph bounding box
/// approximation until a CFF charstring interpreter is added.
///
/// Typical usage:
///   TextImageRenderer renderer(font_loader);
///   ImageBuffer img = renderer.render_text(font, "Hello", style);
///   // Upload img.pixels to GPU via TextureRegistry
class TextImageRenderer {
public:
    explicit TextImageRenderer(const FontLoader& font_loader);
    ~TextImageRenderer() = default;

    TextImageRenderer(const TextImageRenderer&) = delete;
    TextImageRenderer& operator=(const TextImageRenderer&) = delete;

    /// Render a text string into an RGBA image buffer.
    /// The buffer is sized to fit the rendered text.
    ImageBuffer render_text(FontHandle font, const std::string& text,
                            const TextStyle& style = {});

    /// Render a text string into a fixed-size RGBA image buffer.
    /// Text is positioned according to alignment settings in TextStyle.
    ImageBuffer render_text_fixed(FontHandle font, const std::string& text,
                                  uint32_t width, uint32_t height,
                                  const TextStyle& style = {});

    /// Measure text dimensions without rendering.
    /// Returns {width, height} in pixels.
    struct TextExtent {
        float width  = 0.0f;
        float height = 0.0f;
        float ascender  = 0.0f;
        float descender = 0.0f;
    };

    TextExtent measure_text(FontHandle font, const std::string& text,
                            const TextStyle& style = {}) const;

    /// Render a single glyph into an alpha-channel image buffer.
    ImageBuffer render_glyph(FontHandle font, uint32_t codepoint,
                             float font_size);

private:
    /// Rasterize a single glyph outline into an alpha bitmap.
    /// Returns an alpha-only (1-channel) ImageBuffer.
    ImageBuffer rasterize_glyph(FontHandle font, uint32_t codepoint,
                                float font_size, GlyphMetrics& out_metrics);

    /// Composite an alpha glyph onto an RGBA target at (x, y) with given color.
    void composite_glyph(ImageBuffer& target, const ImageBuffer& glyph,
                         int32_t x, int32_t y, const float4& color);

    /// Decode UTF-8 string to codepoints
    static std::vector<uint32_t> utf8_to_codepoints(const std::string& text);

    /// Break text into lines (respecting max_width / word_wrap)
    struct TextLine {
        std::vector<uint32_t> codepoints;
        float width = 0.0f;
    };

    std::vector<TextLine> break_into_lines(FontHandle font,
                                           const std::vector<uint32_t>& codepoints,
                                           const TextStyle& style) const;

    const FontLoader& font_loader_;
};

} // namespace pictor
