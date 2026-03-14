#include "pictor/text/text_image_renderer.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace pictor {

// ============================================================
// Construction
// ============================================================

TextImageRenderer::TextImageRenderer(const FontLoader& font_loader)
    : font_loader_(font_loader) {}

// ============================================================
// UTF-8 decoding
// ============================================================

std::vector<uint32_t> TextImageRenderer::utf8_to_codepoints(const std::string& text) {
    std::vector<uint32_t> result;
    result.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        uint32_t cp = 0;
        uint8_t ch = static_cast<uint8_t>(text[i]);
        if (ch < 0x80) {
            cp = ch; i += 1;
        } else if ((ch & 0xE0) == 0xC0) {
            cp = ch & 0x1F;
            if (i + 1 < text.size()) cp = (cp << 6) | (static_cast<uint8_t>(text[i+1]) & 0x3F);
            i += 2;
        } else if ((ch & 0xF0) == 0xE0) {
            cp = ch & 0x0F;
            if (i + 1 < text.size()) cp = (cp << 6) | (static_cast<uint8_t>(text[i+1]) & 0x3F);
            if (i + 2 < text.size()) cp = (cp << 6) | (static_cast<uint8_t>(text[i+2]) & 0x3F);
            i += 3;
        } else if ((ch & 0xF8) == 0xF0) {
            cp = ch & 0x07;
            if (i + 1 < text.size()) cp = (cp << 6) | (static_cast<uint8_t>(text[i+1]) & 0x3F);
            if (i + 2 < text.size()) cp = (cp << 6) | (static_cast<uint8_t>(text[i+2]) & 0x3F);
            if (i + 3 < text.size()) cp = (cp << 6) | (static_cast<uint8_t>(text[i+3]) & 0x3F);
            i += 4;
        } else {
            cp = 0xFFFD; // Replacement character
            i += 1;
        }
        result.push_back(cp);
    }
    return result;
}

// ============================================================
// Line breaking
// ============================================================

std::vector<TextImageRenderer::TextLine> TextImageRenderer::break_into_lines(
    FontHandle font, const std::vector<uint32_t>& codepoints,
    const TextStyle& style) const {

    std::vector<TextLine> lines;
    TextLine current_line;
    float line_width = 0.0f;
    float space_width = 0.0f;
    size_t last_break_pos = 0;

    // Measure space width
    GlyphMetrics space_m;
    if (font_loader_.get_glyph_metrics(font, ' ', style.font_size, space_m)) {
        space_width = space_m.advance_x + style.letter_spacing;
    }

    for (size_t i = 0; i < codepoints.size(); ++i) {
        uint32_t cp = codepoints[i];

        // Newline
        if (cp == '\n') {
            current_line.width = line_width;
            lines.push_back(std::move(current_line));
            current_line = {};
            line_width = 0.0f;
            continue;
        }

        GlyphMetrics gm;
        float advance = space_width; // fallback
        if (font_loader_.get_glyph_metrics(font, cp, style.font_size, gm)) {
            advance = gm.advance_x + style.letter_spacing;
        }

        // Word wrap check
        if (style.max_width > 0.0f && style.word_wrap &&
            line_width + advance > style.max_width &&
            !current_line.codepoints.empty()) {

            // Try to break at last space
            if (cp == ' ') {
                current_line.width = line_width;
                lines.push_back(std::move(current_line));
                current_line = {};
                line_width = 0.0f;
                continue;
            }

            // Find last space in current line for word break
            bool found_break = false;
            for (size_t j = current_line.codepoints.size(); j > 0; --j) {
                if (current_line.codepoints[j - 1] == ' ') {
                    TextLine before;
                    before.codepoints.assign(current_line.codepoints.begin(),
                                             current_line.codepoints.begin() +
                                             static_cast<ptrdiff_t>(j - 1));
                    // Recalculate width
                    float w = 0.0f;
                    for (auto c : before.codepoints) {
                        GlyphMetrics m;
                        if (font_loader_.get_glyph_metrics(font, c, style.font_size, m))
                            w += m.advance_x + style.letter_spacing;
                    }
                    before.width = w;
                    lines.push_back(std::move(before));

                    TextLine after;
                    after.codepoints.assign(current_line.codepoints.begin() +
                                            static_cast<ptrdiff_t>(j),
                                            current_line.codepoints.end());
                    current_line = std::move(after);
                    line_width = 0.0f;
                    for (auto c : current_line.codepoints) {
                        GlyphMetrics m;
                        if (font_loader_.get_glyph_metrics(font, c, style.font_size, m))
                            line_width += m.advance_x + style.letter_spacing;
                    }
                    found_break = true;
                    break;
                }
            }

            if (!found_break) {
                // Force break at current position
                current_line.width = line_width;
                lines.push_back(std::move(current_line));
                current_line = {};
                line_width = 0.0f;
            }
        }

        current_line.codepoints.push_back(cp);
        line_width += advance;
        if (cp == ' ') last_break_pos = current_line.codepoints.size();
    }

    // Last line
    if (!current_line.codepoints.empty()) {
        current_line.width = line_width;
        lines.push_back(std::move(current_line));
    }

    (void)last_break_pos;
    return lines;
}

// ============================================================
// Text measurement
// ============================================================

TextImageRenderer::TextExtent TextImageRenderer::measure_text(
    FontHandle font, const std::string& text, const TextStyle& style) const {

    auto codepoints = utf8_to_codepoints(text);
    auto lines = break_into_lines(font, codepoints, style);

    const FontMetrics* fm = font_loader_.get_metrics(font);
    float scale = (fm && fm->units_per_em > 0)
                  ? style.font_size / static_cast<float>(fm->units_per_em)
                  : 1.0f;

    TextExtent ext;
    ext.ascender  = fm ? fm->ascender * scale : style.font_size;
    ext.descender = fm ? fm->descender * scale : 0.0f;

    float line_h = fm ? fm->line_height * scale * style.line_spacing
                      : style.font_size * style.line_spacing;

    float max_width = 0.0f;
    for (const auto& line : lines) {
        max_width = std::max(max_width, line.width);
    }

    ext.width  = max_width;
    ext.height = lines.empty() ? 0.0f
                 : line_h * (static_cast<float>(lines.size()) - 1.0f) +
                   ext.ascender - ext.descender;

    return ext;
}

// ============================================================
// Glyph rasterization (simple scanline approach)
// ============================================================

ImageBuffer TextImageRenderer::rasterize_glyph(FontHandle font,
                                                uint32_t codepoint,
                                                float font_size,
                                                GlyphMetrics& out_metrics) {
    ImageBuffer result;

    if (!font_loader_.get_glyph_metrics(font, codepoint, font_size, out_metrics)) {
        return result;
    }

    uint32_t w = static_cast<uint32_t>(std::ceil(out_metrics.width));
    uint32_t h = static_cast<uint32_t>(std::ceil(out_metrics.height));
    if (w == 0 || h == 0) {
        w = std::max(w, 1u);
        h = std::max(h, 1u);
    }

    // Clamp to reasonable size to prevent excessive memory usage
    w = std::min(w, 4096u);
    h = std::min(h, 4096u);

    result.allocate(w, h, 1);

    // Simple box rasterization: fill the glyph bounding box with full alpha.
    // A production implementation would parse glyph contours from 'glyf' table
    // and rasterize using scanline coverage / SDF generation.
    // For now this provides a functional placeholder that correctly computes
    // metrics and layout while actual outline rasterization is added incrementally.
    const FontTableEntry* entry = font_loader_.get_entry(font);
    if (!entry) return result;

    // Fill with solid alpha (simple box glyph)
    // Glyph shape is approximated as a filled rectangle.
    // Whitespace characters get no fill.
    if (codepoint > ' ') {
        uint32_t pad_x = w > 2 ? 1 : 0;
        uint32_t pad_y = h > 2 ? 1 : 0;
        for (uint32_t py = pad_y; py < h - pad_y; ++py) {
            for (uint32_t px = pad_x; px < w - pad_x; ++px) {
                result.pixels[py * w + px] = 255;
            }
        }
    }

    return result;
}

ImageBuffer TextImageRenderer::render_glyph(FontHandle font, uint32_t codepoint,
                                             float font_size) {
    GlyphMetrics metrics;
    ImageBuffer alpha = rasterize_glyph(font, codepoint, font_size, metrics);
    if (alpha.pixels.empty()) return alpha;

    // Convert alpha to RGBA (white text)
    ImageBuffer rgba;
    rgba.allocate(alpha.width, alpha.height, 4);
    for (uint32_t i = 0; i < alpha.width * alpha.height; ++i) {
        rgba.pixels[i * 4 + 0] = 255;
        rgba.pixels[i * 4 + 1] = 255;
        rgba.pixels[i * 4 + 2] = 255;
        rgba.pixels[i * 4 + 3] = alpha.pixels[i];
    }
    return rgba;
}

// ============================================================
// Glyph compositing
// ============================================================

void TextImageRenderer::composite_glyph(ImageBuffer& target,
                                         const ImageBuffer& glyph,
                                         int32_t x, int32_t y,
                                         const float4& color) {
    for (uint32_t gy = 0; gy < glyph.height; ++gy) {
        int32_t ty = y + static_cast<int32_t>(gy);
        if (ty < 0 || ty >= static_cast<int32_t>(target.height)) continue;

        for (uint32_t gx = 0; gx < glyph.width; ++gx) {
            int32_t tx = x + static_cast<int32_t>(gx);
            if (tx < 0 || tx >= static_cast<int32_t>(target.width)) continue;

            uint8_t alpha;
            if (glyph.channels == 1) {
                alpha = glyph.pixels[gy * glyph.width + gx];
            } else {
                alpha = glyph.pixels[(gy * glyph.width + gx) * glyph.channels + 3];
            }

            if (alpha == 0) continue;

            float a = (static_cast<float>(alpha) / 255.0f) * color.w;
            size_t tidx = (static_cast<size_t>(ty) * target.width +
                           static_cast<size_t>(tx)) * 4;

            // Alpha blending (premultiplied)
            float src_r = color.x * a * 255.0f;
            float src_g = color.y * a * 255.0f;
            float src_b = color.z * a * 255.0f;
            float dst_a = static_cast<float>(target.pixels[tidx + 3]) / 255.0f;
            float inv_a = 1.0f - a;

            target.pixels[tidx + 0] = static_cast<uint8_t>(std::min(
                src_r + target.pixels[tidx + 0] * inv_a, 255.0f));
            target.pixels[tidx + 1] = static_cast<uint8_t>(std::min(
                src_g + target.pixels[tidx + 1] * inv_a, 255.0f));
            target.pixels[tidx + 2] = static_cast<uint8_t>(std::min(
                src_b + target.pixels[tidx + 2] * inv_a, 255.0f));
            target.pixels[tidx + 3] = static_cast<uint8_t>(std::min(
                (a + dst_a * inv_a) * 255.0f, 255.0f));
        }
    }
}

// ============================================================
// Text rendering
// ============================================================

ImageBuffer TextImageRenderer::render_text(FontHandle font,
                                           const std::string& text,
                                           const TextStyle& style) {
    auto extent = measure_text(font, text, style);
    uint32_t w = static_cast<uint32_t>(std::ceil(extent.width)) + 2;
    uint32_t h = static_cast<uint32_t>(std::ceil(extent.height)) + 2;
    if (w == 0) w = 1;
    if (h == 0) h = 1;

    return render_text_fixed(font, text, w, h, style);
}

ImageBuffer TextImageRenderer::render_text_fixed(FontHandle font,
                                                  const std::string& text,
                                                  uint32_t width, uint32_t height,
                                                  const TextStyle& style) {
    ImageBuffer target;
    target.allocate(width, height, 4);

    auto codepoints = utf8_to_codepoints(text);
    auto lines = break_into_lines(font, codepoints, style);

    const FontMetrics* fm = font_loader_.get_metrics(font);
    float scale = (fm && fm->units_per_em > 0)
                  ? style.font_size / static_cast<float>(fm->units_per_em)
                  : 1.0f;

    float ascender = fm ? fm->ascender * scale : style.font_size * 0.8f;
    float line_h = fm ? fm->line_height * scale * style.line_spacing
                      : style.font_size * style.line_spacing;

    // Compute total text height for vertical alignment
    float total_h = lines.empty() ? 0.0f
                    : line_h * static_cast<float>(lines.size());

    float start_y = 0.0f;
    switch (style.align_v) {
        case TextAlignV::TOP:      start_y = 0.0f; break;
        case TextAlignV::MIDDLE:   start_y = (static_cast<float>(height) - total_h) * 0.5f; break;
        case TextAlignV::BOTTOM:   start_y = static_cast<float>(height) - total_h; break;
        case TextAlignV::BASELINE: start_y = 0.0f; break;
    }

    for (size_t line_idx = 0; line_idx < lines.size(); ++line_idx) {
        const auto& line = lines[line_idx];
        float cursor_y = start_y + static_cast<float>(line_idx) * line_h + ascender;

        // Horizontal alignment
        float cursor_x = 0.0f;
        switch (style.align_h) {
            case TextAlignH::LEFT:   cursor_x = 0.0f; break;
            case TextAlignH::CENTER: cursor_x = (static_cast<float>(width) - line.width) * 0.5f; break;
            case TextAlignH::RIGHT:  cursor_x = static_cast<float>(width) - line.width; break;
        }

        for (size_t ci = 0; ci < line.codepoints.size(); ++ci) {
            uint32_t cp = line.codepoints[ci];
            GlyphMetrics gm;
            ImageBuffer glyph_img = rasterize_glyph(font, cp, style.font_size, gm);

            if (!glyph_img.pixels.empty()) {
                int32_t gx = static_cast<int32_t>(cursor_x + gm.bearing_x);
                int32_t gy = static_cast<int32_t>(cursor_y - gm.bearing_y);

                composite_glyph(target, glyph_img, gx, gy, style.color);
            }

            cursor_x += gm.advance_x + style.letter_spacing;

            // Kerning
            if (ci + 1 < line.codepoints.size()) {
                int16_t kern = font_loader_.get_kerning(
                    font, cp, line.codepoints[ci + 1]);
                cursor_x += kern * scale;
            }
        }
    }

    return target;
}

} // namespace pictor
