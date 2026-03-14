#include "pictor/text/text_svg_renderer.h"
#include <cmath>
#include <sstream>
#include <iomanip>

namespace pictor {

// ============================================================
// Big-endian read helpers
// ============================================================

namespace {

inline uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline int16_t read_i16(const uint8_t* p) {
    return static_cast<int16_t>(read_u16(p));
}

inline uint32_t read_u32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
            static_cast<uint32_t>(p[3]);
}

inline uint32_t make_tag(char a, char b, char c, char d) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) <<  8) |
            static_cast<uint32_t>(static_cast<uint8_t>(d));
}

} // anonymous namespace

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

    GlyphOutline outline;
    outline.codepoint = codepoint;
    outline.em_size = static_cast<float>(entry.metrics.units_per_em);

    const uint8_t* data = entry.raw_data.data();
    size_t data_size = entry.raw_data.size();

    // Find glyf and loca tables
    // We need to scan the table directory again since we don't cache offsets
    // in FontTableEntry (they're in ParsedFontData which is private to FontLoader).
    uint32_t glyf_offset = 0, glyf_length = 0;
    uint32_t loca_offset = 0, loca_length = 0;
    uint16_t num_glyphs = 0;
    int16_t  loc_format = 0;

    if (data_size < 12) return outline;

    uint16_t num_tables = read_u16(data + 4);
    for (uint16_t t = 0; t < num_tables; ++t) {
        size_t rec = 12 + t * 16;
        if (rec + 16 > data_size) break;
        uint32_t tag = read_u32(data + rec);
        uint32_t off = read_u32(data + rec + 8);
        uint32_t len = read_u32(data + rec + 12);
        if (tag == make_tag('g','l','y','f'))      { glyf_offset = off; glyf_length = len; }
        else if (tag == make_tag('l','o','c','a')) { loca_offset = off; loca_length = len; }
        else if (tag == make_tag('m','a','x','p') && off + 6 <= data_size) {
            num_glyphs = read_u16(data + off + 4);
        }
        else if (tag == make_tag('h','e','a','d') && off + 54 <= data_size) {
            loc_format = read_i16(data + off + 50);
        }
    }

    if (glyf_offset == 0 || loca_offset == 0 || glyph_index >= num_glyphs) {
        return outline;
    }

    // Get glyph offset from 'loca' table
    uint32_t glyph_off, glyph_end;
    if (loc_format == 0) {
        // Short format: offsets are uint16 * 2
        uint32_t li = loca_offset + glyph_index * 2;
        uint32_t li_next = li + 2;
        if (li_next + 2 > data_size) return outline;
        glyph_off = static_cast<uint32_t>(read_u16(data + li)) * 2;
        glyph_end = static_cast<uint32_t>(read_u16(data + li_next)) * 2;
    } else {
        // Long format: offsets are uint32
        uint32_t li = loca_offset + glyph_index * 4;
        uint32_t li_next = li + 4;
        if (li_next + 4 > data_size) return outline;
        glyph_off = read_u32(data + li);
        glyph_end = read_u32(data + li_next);
    }

    if (glyph_off == glyph_end) {
        // Empty glyph (e.g., space)
        return outline;
    }

    uint32_t abs_off = glyf_offset + glyph_off;
    if (abs_off + 10 > data_size) return outline;

    const uint8_t* gdata = data + abs_off;
    int16_t num_contours = read_i16(gdata);

    if (num_contours < 0) {
        // Composite glyph — skip for now (would need recursive decomposition)
        return outline;
    }

    if (num_contours == 0) return outline;

    // Simple glyph
    uint32_t off = 10; // Skip header (numContours + xMin/yMin/xMax/yMax)

    // Read end points of each contour
    std::vector<uint16_t> end_points(num_contours);
    for (int16_t c = 0; c < num_contours; ++c) {
        if (abs_off + off + 2 > data_size) return outline;
        end_points[c] = read_u16(gdata + off);
        off += 2;
    }

    uint16_t total_points = end_points.back() + 1;

    // Skip instructions
    if (abs_off + off + 2 > data_size) return outline;
    uint16_t instr_len = read_u16(gdata + off);
    off += 2 + instr_len;

    // Read flags
    std::vector<uint8_t> flags(total_points);
    for (uint16_t i = 0; i < total_points;) {
        if (abs_off + off >= data_size) return outline;
        uint8_t flag = gdata[off++];
        flags[i++] = flag;
        if (flag & 0x08) { // Repeat flag
            if (abs_off + off >= data_size) return outline;
            uint8_t repeat = gdata[off++];
            for (uint8_t r = 0; r < repeat && i < total_points; ++r) {
                flags[i++] = flag;
            }
        }
    }

    // Read X coordinates
    std::vector<int16_t> x_coords(total_points, 0);
    int16_t x_val = 0;
    for (uint16_t i = 0; i < total_points; ++i) {
        if (flags[i] & 0x02) { // Short X
            if (abs_off + off >= data_size) return outline;
            uint8_t dx = gdata[off++];
            x_val += (flags[i] & 0x10) ? dx : -static_cast<int16_t>(dx);
        } else if (!(flags[i] & 0x10)) { // Long X (not same)
            if (abs_off + off + 1 >= data_size) return outline;
            x_val += read_i16(gdata + off);
            off += 2;
        }
        // else: same as previous (x_val unchanged)
        x_coords[i] = x_val;
    }

    // Read Y coordinates
    std::vector<int16_t> y_coords(total_points, 0);
    int16_t y_val = 0;
    for (uint16_t i = 0; i < total_points; ++i) {
        if (flags[i] & 0x04) { // Short Y
            if (abs_off + off >= data_size) return outline;
            uint8_t dy = gdata[off++];
            y_val += (flags[i] & 0x20) ? dy : -static_cast<int16_t>(dy);
        } else if (!(flags[i] & 0x20)) { // Long Y (not same)
            if (abs_off + off + 1 >= data_size) return outline;
            y_val += read_i16(gdata + off);
            off += 2;
        }
        y_coords[i] = y_val;
    }

    // Convert to SVG path commands
    uint16_t contour_start = 0;
    for (int16_t c = 0; c < num_contours; ++c) {
        uint16_t contour_end = end_points[c];

        // Find first on-curve point
        uint16_t first = contour_start;
        bool first_on_curve = (flags[first] & 0x01) != 0;

        if (first_on_curve) {
            SvgPathPoint move;
            move.command = SvgPathCommand::MOVE_TO;
            move.x = static_cast<float>(x_coords[first]);
            move.y = static_cast<float>(y_coords[first]);
            outline.path.push_back(move);
        } else {
            // First point is off-curve; use midpoint between first and last off-curve
            bool last_on = (flags[contour_end] & 0x01) != 0;
            float mx, my;
            if (last_on) {
                mx = static_cast<float>(x_coords[contour_end]);
                my = static_cast<float>(y_coords[contour_end]);
            } else {
                mx = (static_cast<float>(x_coords[first]) +
                      static_cast<float>(x_coords[contour_end])) * 0.5f;
                my = (static_cast<float>(y_coords[first]) +
                      static_cast<float>(y_coords[contour_end])) * 0.5f;
            }
            SvgPathPoint move;
            move.command = SvgPathCommand::MOVE_TO;
            move.x = mx;
            move.y = my;
            outline.path.push_back(move);
        }

        // Process remaining points
        for (uint16_t i = contour_start; i <= contour_end; ++i) {
            uint16_t curr = i;
            uint16_t next = (i < contour_end) ? i + 1 : contour_start;

            bool curr_on = (flags[curr] & 0x01) != 0;
            bool next_on = (flags[next] & 0x01) != 0;

            if (curr_on && next_on) {
                // Line to
                SvgPathPoint pt;
                pt.command = SvgPathCommand::LINE_TO;
                pt.x = static_cast<float>(x_coords[next]);
                pt.y = static_cast<float>(y_coords[next]);
                outline.path.push_back(pt);
            } else if (curr_on && !next_on) {
                // Current is on-curve, next is off-curve control point
                uint16_t next_next = (next < contour_end) ? next + 1 : contour_start;
                bool nn_on = (flags[next_next] & 0x01) != 0;

                SvgPathPoint pt;
                pt.command = SvgPathCommand::QUAD_TO;
                pt.cx = static_cast<float>(x_coords[next]);
                pt.cy = static_cast<float>(y_coords[next]);

                if (nn_on) {
                    pt.x = static_cast<float>(x_coords[next_next]);
                    pt.y = static_cast<float>(y_coords[next_next]);
                    i = next; // Skip the off-curve point
                } else {
                    // Implied on-curve point at midpoint
                    pt.x = (static_cast<float>(x_coords[next]) +
                            static_cast<float>(x_coords[next_next])) * 0.5f;
                    pt.y = (static_cast<float>(y_coords[next]) +
                            static_cast<float>(y_coords[next_next])) * 0.5f;
                    i = next; // Skip the off-curve point
                }
                outline.path.push_back(pt);
            } else if (!curr_on) {
                // Two consecutive off-curve points: implied on-curve at midpoint
                // This case is handled by the curr_on && !next_on branch above
                // through the midpoint calculation. Skip here.
            }
        }

        // Close contour
        SvgPathPoint close;
        close.command = SvgPathCommand::CLOSE;
        outline.path.push_back(close);

        contour_start = contour_end + 1;
    }

    return outline;
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
