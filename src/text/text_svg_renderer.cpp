#include "pictor/text/text_svg_renderer.h"
#include "truetype_outline.h"
#include <cmath>
#include <sstream>
#include <iomanip>

namespace pictor {

// ============================================================
// Construction
// ============================================================

TextSvgRenderer::TextSvgRenderer(const FontLoader& font_loader)
    : font_loader_(font_loader) {}

// ============================================================
// UTF-8 decoding
// ============================================================

std::vector<uint32_t> TextSvgRenderer::utf8_to_codepoints(const std::string& text) {
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
            cp = 0xFFFD;
            i += 1;
        }
        result.push_back(cp);
    }
    return result;
}

// ============================================================
// Formatting helpers
// ============================================================

std::string TextSvgRenderer::fmt(float v) {
    if (v == std::floor(v) && std::abs(v) < 1e6f) {
        return std::to_string(static_cast<int>(v));
    }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << v;
    std::string s = ss.str();
    // Remove trailing zeros
    size_t dot = s.find('.');
    if (dot != std::string::npos) {
        size_t last_nonzero = s.find_last_not_of('0');
        if (last_nonzero == dot) {
            s.erase(dot);
        } else if (last_nonzero != std::string::npos) {
            s.erase(last_nonzero + 1);
        }
    }
    return s;
}

std::string TextSvgRenderer::color_to_hex(const float4& color) {
    auto to_hex = [](float v) -> uint8_t {
        return static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, v)) * 255.0f);
    };
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                  to_hex(color.x), to_hex(color.y), to_hex(color.z));
    return buf;
}

// ============================================================
// TrueType glyph outline parsing
// ============================================================

GlyphOutline TextSvgRenderer::parse_truetype_glyph(
    const FontTableEntry& entry, uint16_t glyph_index,
    uint32_t codepoint) const {

    return detail::decode_truetype_outline(entry, glyph_index, codepoint);
}

// ============================================================
// Glyph outline extraction
// ============================================================

GlyphOutline TextSvgRenderer::extract_glyph_outline(FontHandle font,
                                                      uint32_t codepoint) const {
    const FontTableEntry* entry = font_loader_.get_entry(font);
    if (!entry) return {};

    uint16_t glyph_index = font_loader_.codepoint_to_glyph_index(font, codepoint);
    if (glyph_index == 0 && codepoint != 0) return {};

    GlyphOutline outline = parse_truetype_glyph(*entry, glyph_index, codepoint);

    // Set advance
    GlyphMetrics gm;
    if (font_loader_.get_glyph_metrics(font, codepoint,
                                        static_cast<float>(entry->metrics.units_per_em), gm)) {
        outline.advance_x = gm.advance_x;
    }

    return outline;
}

std::vector<GlyphOutline> TextSvgRenderer::extract_charset_outlines(
    FontHandle font, CharSet charset) const {

    std::vector<GlyphOutline> outlines;
    auto ranges = charset_to_ranges(charset);

    for (const auto& range : ranges) {
        for (uint32_t cp = range.begin; cp <= range.end; ++cp) {
            if (!font_loader_.has_glyph(font, cp)) continue;
            auto outline = extract_glyph_outline(font, cp);
            if (!outline.path.empty()) {
                outlines.push_back(std::move(outline));
            }
        }
    }

    return outlines;
}

// ============================================================
// SVG path string generation
// ============================================================

std::string TextSvgRenderer::outline_to_svg_path(const GlyphOutline& outline,
                                                   float scale,
                                                   float offset_x,
                                                   float offset_y) {
    std::ostringstream ss;

    for (const auto& pt : outline.path) {
        float x  = pt.x  * scale + offset_x;
        float y  = pt.y  * scale + offset_y;
        float cx = pt.cx  * scale + offset_x;
        float cy = pt.cy  * scale + offset_y;
        float cx2 = pt.cx2 * scale + offset_x;
        float cy2 = pt.cy2 * scale + offset_y;

        switch (pt.command) {
            case SvgPathCommand::MOVE_TO:
                ss << "M" << fmt(x) << " " << fmt(y);
                break;
            case SvgPathCommand::LINE_TO:
                ss << "L" << fmt(x) << " " << fmt(y);
                break;
            case SvgPathCommand::QUAD_TO:
                ss << "Q" << fmt(cx) << " " << fmt(cy) << " "
                   << fmt(x)  << " " << fmt(y);
                break;
            case SvgPathCommand::CUBIC_TO:
                ss << "C" << fmt(cx)  << " " << fmt(cy)  << " "
                   << fmt(cx2) << " " << fmt(cy2) << " "
                   << fmt(x)   << " " << fmt(y);
                break;
            case SvgPathCommand::CLOSE:
                ss << "Z";
                break;
        }
    }

    return ss.str();
}

// ============================================================
// Full SVG document generation
// ============================================================

std::string TextSvgRenderer::render_glyph_svg(FontHandle font,
                                                uint32_t codepoint,
                                                float size) const {
    auto outline = extract_glyph_outline(font, codepoint);
    if (outline.path.empty()) return "";

    float scale = (outline.em_size > 0) ? size / outline.em_size : 1.0f;

    const FontMetrics* fm = font_loader_.get_metrics(font);
    float ascender = fm ? fm->ascender * scale : size * 0.8f;
    float descender = fm ? fm->descender * scale : size * -0.2f;
    float total_h = ascender - descender;
    float total_w = outline.advance_x * scale;

    float pad = 4.0f;
    float svg_w = total_w + pad * 2;
    float svg_h = total_h + pad * 2;

    std::ostringstream ss;
    ss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    ss << "<svg xmlns=\"http://www.w3.org/2000/svg\" "
       << "width=\"" << fmt(svg_w) << "\" height=\"" << fmt(svg_h) << "\" "
       << "viewBox=\"0 0 " << fmt(svg_w) << " " << fmt(svg_h) << "\">\n";

    // Flip Y: font Y-up → SVG Y-down via transform
    float ty = ascender + pad;
    ss << "  <g transform=\"translate(" << fmt(pad) << "," << fmt(ty) << ") scale("
       << fmt(scale) << "," << fmt(-scale) << ")\">\n";
    ss << "    <path d=\"" << outline_to_svg_path(outline) << "\" fill=\"black\"/>\n";
    ss << "  </g>\n";
    ss << "</svg>\n";

    return ss.str();
}

std::string TextSvgRenderer::render_text_svg(FontHandle font,
                                              const std::string& text,
                                              const TextStyle& style) const {
    SvgOptions opts;
    return render_text_svg(font, text, style, opts);
}

std::string TextSvgRenderer::render_text_svg(FontHandle font,
                                              const std::string& text,
                                              const TextStyle& style,
                                              const SvgOptions& options) const {
    auto codepoints = utf8_to_codepoints(text);
    if (codepoints.empty()) return "";

    const FontMetrics* fm = font_loader_.get_metrics(font);
    float em = (fm) ? static_cast<float>(fm->units_per_em) : 1000.0f;
    float scale = style.font_size / em;
    float ascender = fm ? fm->ascender * scale : style.font_size * 0.8f;
    float descender = fm ? fm->descender * scale : style.font_size * -0.2f;
    float line_h = fm ? fm->line_height * scale * style.line_spacing
                      : style.font_size * style.line_spacing;

    // Split into lines on '\n'
    std::vector<std::vector<uint32_t>> lines;
    lines.push_back({});
    for (auto cp : codepoints) {
        if (cp == '\n') {
            lines.push_back({});
        } else {
            lines.back().push_back(cp);
        }
    }

    // Measure each line
    struct LineMeasure {
        float width = 0.0f;
        std::vector<std::pair<uint32_t, float>> glyphs; // (codepoint, advance)
    };
    std::vector<LineMeasure> measured;
    float max_width = 0.0f;

    for (const auto& line : lines) {
        LineMeasure lm;
        float cursor = 0.0f;
        for (size_t i = 0; i < line.size(); ++i) {
            GlyphMetrics gm;
            float adv = style.font_size * 0.5f; // fallback
            if (font_loader_.get_glyph_metrics(font, line[i], style.font_size, gm)) {
                adv = gm.advance_x;
            }
            adv += style.letter_spacing;
            lm.glyphs.push_back({line[i], adv});
            cursor += adv;

            // Kerning
            if (i + 1 < line.size()) {
                int16_t kern = font_loader_.get_kerning(font, line[i], line[i + 1]);
                cursor += kern * scale;
            }
        }
        lm.width = cursor;
        max_width = std::max(max_width, cursor);
        measured.push_back(std::move(lm));
    }

    float pad = options.padding;
    float svg_w = max_width + pad * 2;
    float svg_h = line_h * static_cast<float>(lines.size()) + pad * 2;

    std::ostringstream ss;

    if (options.include_xml_header) {
        ss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    }

    ss << "<svg xmlns=\"http://www.w3.org/2000/svg\"";
    if (!options.css_class.empty()) {
        ss << " class=\"" << options.css_class << "\"";
    }
    ss << " width=\"" << fmt(svg_w) << "\" height=\"" << fmt(svg_h) << "\""
       << " viewBox=\"0 0 " << fmt(svg_w) << " " << fmt(svg_h) << "\">\n";

    if (options.include_background) {
        ss << "  <rect width=\"100%\" height=\"100%\" fill=\""
           << color_to_hex(options.background_color)
           << "\" opacity=\"" << fmt(options.background_color.w) << "\"/>\n";
    }

    std::string fill_color = color_to_hex(style.color);
    float fill_opacity = style.color.w;

    for (size_t li = 0; li < measured.size(); ++li) {
        const auto& lm = measured[li];

        // Horizontal alignment
        float start_x = pad;
        switch (style.align_h) {
            case TextAlignH::LEFT:   start_x = pad; break;
            case TextAlignH::CENTER: start_x = pad + (max_width - lm.width) * 0.5f; break;
            case TextAlignH::RIGHT:  start_x = pad + max_width - lm.width; break;
        }

        float baseline_y = pad + ascender + static_cast<float>(li) * line_h;
        float cursor_x = start_x;

        for (const auto& [cp, adv] : lm.glyphs) {
            auto outline = extract_glyph_outline(font, cp);
            if (!outline.path.empty()) {
                float gy = options.flip_y ? baseline_y : baseline_y;
                float y_scale = options.flip_y ? -scale : scale;

                ss << "  <path d=\""
                   << outline_to_svg_path(outline, 1.0f)
                   << "\" fill=\"" << fill_color << "\"";
                if (fill_opacity < 1.0f) {
                    ss << " opacity=\"" << fmt(fill_opacity) << "\"";
                }
                ss << " transform=\"translate(" << fmt(cursor_x) << ","
                   << fmt(gy) << ") scale(" << fmt(scale)
                   << "," << fmt(y_scale) << ")\"";
                ss << "/>\n";
            }
            cursor_x += adv;
        }
    }

    ss << "</svg>\n";
    return ss.str();
}

} // namespace pictor
