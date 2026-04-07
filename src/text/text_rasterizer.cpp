#include "pictor/text/text_rasterizer.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace pictor {

// ============================================================
// TrueType outline parsing and scanline rasterization
// ============================================================

namespace {

// Big-endian read helpers (TrueType is always big-endian)
inline uint16_t rd_u16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
inline int16_t rd_i16(const uint8_t* p) {
    return static_cast<int16_t>(rd_u16(p));
}
inline uint32_t rd_u32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
            static_cast<uint32_t>(p[3]);
}
inline uint32_t mk_tag(char a, char b, char c, char d) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) <<  8) |
            static_cast<uint32_t>(static_cast<uint8_t>(d));
}

// Directed edge for scanline rasterization (y0 < y1 always)
struct RasterEdge {
    float x0, y0, x1, y1;
    int direction; // +1 or -1 (original winding direction)
};

void add_edge(std::vector<RasterEdge>& edges,
              float x0, float y0, float x1, float y1) {
    if (std::abs(y1 - y0) < 0.001f) return; // skip horizontal
    if (y0 < y1) {
        edges.push_back({x0, y0, x1, y1, 1});
    } else {
        edges.push_back({x1, y1, x0, y0, -1});
    }
}

void flatten_quad_bezier(std::vector<RasterEdge>& edges,
                         float x0, float y0,
                         float cx, float cy,
                         float x1, float y1,
                         float tolerance = 0.25f) {
    float mx = (x0 + x1) * 0.5f;
    float my = (y0 + y1) * 0.5f;
    float dx = cx - mx;
    float dy = cy - my;
    if (dx * dx + dy * dy <= tolerance * tolerance) {
        add_edge(edges, x0, y0, x1, y1);
        return;
    }
    float x01  = (x0 + cx) * 0.5f, y01  = (y0 + cy) * 0.5f;
    float x12  = (cx + x1) * 0.5f, y12  = (cy + y1) * 0.5f;
    float x012 = (x01 + x12) * 0.5f, y012 = (y01 + y12) * 0.5f;
    flatten_quad_bezier(edges, x0, y0, x01, y01, x012, y012, tolerance);
    flatten_quad_bezier(edges, x012, y012, x12, y12, x1, y1, tolerance);
}

// Parse 'glyf' table contours and return directed edges in pixel coordinates.
// Transform: pixel_x = font_x * scale, pixel_y = ascender_px - font_y * scale
bool extract_glyph_edges(const uint8_t* data, size_t data_size,
                          uint16_t glyph_index,
                          float scale, float ascender_px,
                          std::vector<RasterEdge>& out) {
    if (data_size < 12) return false;

    // Locate glyf, loca, maxp, head tables
    uint16_t num_tables = rd_u16(data + 4);
    uint32_t glyf_off = 0, loca_off = 0;
    uint16_t num_glyphs = 0;
    int16_t  loc_fmt = 0;

    for (uint16_t t = 0; t < num_tables; ++t) {
        size_t r = 12 + t * 16;
        if (r + 16 > data_size) break;
        uint32_t tag = rd_u32(data + r);
        uint32_t off = rd_u32(data + r + 8);
        uint32_t len = rd_u32(data + r + 12);
        if (tag == mk_tag('g','l','y','f'))                              glyf_off = off;
        else if (tag == mk_tag('l','o','c','a'))                         loca_off = off;
        else if (tag == mk_tag('m','a','x','p') && off + 6 <= data_size) num_glyphs = rd_u16(data + off + 4);
        else if (tag == mk_tag('h','e','a','d') && off + 54 <= data_size) loc_fmt = rd_i16(data + off + 50);
    }
    if (glyf_off == 0 || loca_off == 0 || glyph_index >= num_glyphs)
        return false;

    // Resolve glyph offset via 'loca'
    uint32_t g_off, g_end;
    if (loc_fmt == 0) {
        uint32_t li = loca_off + glyph_index * 2;
        if (li + 4 > data_size) return false;
        g_off = static_cast<uint32_t>(rd_u16(data + li)) * 2;
        g_end = static_cast<uint32_t>(rd_u16(data + li + 2)) * 2;
    } else {
        uint32_t li = loca_off + glyph_index * 4;
        if (li + 8 > data_size) return false;
        g_off = rd_u32(data + li);
        g_end = rd_u32(data + li + 4);
    }
    if (g_off == g_end) return false; // empty glyph (space etc.)

    uint32_t abs = glyf_off + g_off;
    if (abs + 10 > data_size) return false;

    const uint8_t* g = data + abs;
    int16_t n_contours = rd_i16(g);
    if (n_contours <= 0) return false; // composite or empty

    // End-points array
    uint32_t p = 10;
    std::vector<uint16_t> ends(n_contours);
    for (int16_t c = 0; c < n_contours; ++c) {
        if (abs + p + 2 > data_size) return false;
        ends[c] = rd_u16(g + p);
        p += 2;
    }
    uint16_t total_pts = ends.back() + 1;

    // Skip instructions
    if (abs + p + 2 > data_size) return false;
    uint16_t instr_len = rd_u16(g + p);
    p += 2 + instr_len;

    // Flags
    std::vector<uint8_t> fl(total_pts);
    for (uint16_t i = 0; i < total_pts;) {
        if (abs + p >= data_size) return false;
        uint8_t f = g[p++];
        fl[i++] = f;
        if (f & 0x08) {
            if (abs + p >= data_size) return false;
            uint8_t rep = g[p++];
            for (uint8_t r = 0; r < rep && i < total_pts; ++r) fl[i++] = f;
        }
    }

    // X coordinates (delta-encoded)
    std::vector<int16_t> xs(total_pts, 0);
    int16_t xv = 0;
    for (uint16_t i = 0; i < total_pts; ++i) {
        if (fl[i] & 0x02) {
            if (abs + p >= data_size) return false;
            uint8_t d = g[p++];
            xv += (fl[i] & 0x10) ? d : -static_cast<int16_t>(d);
        } else if (!(fl[i] & 0x10)) {
            if (abs + p + 1 >= data_size) return false;
            xv += rd_i16(g + p); p += 2;
        }
        xs[i] = xv;
    }

    // Y coordinates (delta-encoded)
    std::vector<int16_t> ys(total_pts, 0);
    int16_t yv = 0;
    for (uint16_t i = 0; i < total_pts; ++i) {
        if (fl[i] & 0x04) {
            if (abs + p >= data_size) return false;
            uint8_t d = g[p++];
            yv += (fl[i] & 0x20) ? d : -static_cast<int16_t>(d);
        } else if (!(fl[i] & 0x20)) {
            if (abs + p + 1 >= data_size) return false;
            yv += rd_i16(g + p); p += 2;
        }
        ys[i] = yv;
    }

    // Transform helpers: font-units → pixel coords
    auto tx = [&](float fx) -> float { return fx * scale; };
    auto ty = [&](float fy) -> float { return ascender_px - fy * scale; };

    // Walk contours and emit edges
    uint16_t cstart = 0;
    for (int16_t c = 0; c < n_contours; ++c) {
        uint16_t cend = ends[c];
        if (cend < cstart) break;

        // Determine starting on-curve point
        bool first_on = (fl[cstart] & 0x01) != 0;
        float sx, sy;
        if (first_on) {
            sx = tx(xs[cstart]); sy = ty(ys[cstart]);
        } else {
            bool last_on = (fl[cend] & 0x01) != 0;
            if (last_on) {
                sx = tx(xs[cend]); sy = ty(ys[cend]);
            } else {
                sx = tx((xs[cstart] + xs[cend]) * 0.5f);
                sy = ty((ys[cstart] + ys[cend]) * 0.5f);
            }
        }

        float cx_ = sx, cy_ = sy;
        for (uint16_t i = cstart; i <= cend; ++i) {
            uint16_t nxt = (i < cend) ? i + 1 : cstart;
            bool cur_on = (fl[i] & 0x01) != 0;
            bool nxt_on = (fl[nxt] & 0x01) != 0;

            if (cur_on && nxt_on) {
                float nx = tx(xs[nxt]), ny = ty(ys[nxt]);
                add_edge(out, cx_, cy_, nx, ny);
                cx_ = nx; cy_ = ny;
            } else if (cur_on && !nxt_on) {
                uint16_t nn = (nxt < cend) ? nxt + 1 : cstart;
                bool nn_on = (fl[nn] & 0x01) != 0;
                float cpx = tx(xs[nxt]), cpy = ty(ys[nxt]);
                float ex, ey;
                if (nn_on) {
                    ex = tx(xs[nn]); ey = ty(ys[nn]);
                    i = nxt; // skip off-curve point
                } else {
                    ex = (cpx + tx(xs[nn])) * 0.5f;
                    ey = (cpy + ty(ys[nn])) * 0.5f;
                    i = nxt;
                }
                flatten_quad_bezier(out, cx_, cy_, cpx, cpy, ex, ey);
                cx_ = ex; cy_ = ey;
            }
            // !cur_on case: handled by prior cur_on && !nxt_on
        }
        // Close contour
        add_edge(out, cx_, cy_, sx, sy);
        cstart = cend + 1;
    }

    return !out.empty();
}

// Non-zero-winding scanline rasterization with 4x Y oversampling (AA).
// Writes alpha into an atlas page at (dst_x, dst_y) with glyph size (gw, gh).
void scanline_rasterize(const std::vector<RasterEdge>& edges,
                        uint8_t* pixels, uint32_t page_w, uint32_t channels,
                        uint32_t dst_x, uint32_t dst_y,
                        uint32_t gw, uint32_t gh) {
    if (gw == 0 || gh == 0 || edges.empty()) return;

    constexpr int OVERSAMPLE = 4;
    uint32_t sub_h = gh * OVERSAMPLE;

    // Crossing buffer: winding increment per (sub-scanline, x)
    std::vector<float> crossings(static_cast<size_t>(gw) * sub_h, 0.0f);

    for (const auto& e : edges) {
        float sy0 = e.y0 * OVERSAMPLE;
        float sy1 = e.y1 * OVERSAMPLE;
        int s_start = std::max(0, static_cast<int>(std::ceil(sy0)));
        int s_end   = std::min(static_cast<int>(sub_h),
                               static_cast<int>(std::ceil(sy1)));

        float dy = e.y1 - e.y0;
        if (std::abs(dy) < 0.001f) continue;
        float inv_dy = 1.0f / dy;

        for (int sy = s_start; sy < s_end; ++sy) {
            float y = (static_cast<float>(sy) + 0.5f) /
                      static_cast<float>(OVERSAMPLE);
            float t = (y - e.y0) * inv_dy;
            float x = e.x0 + (e.x1 - e.x0) * t;
            int ix = static_cast<int>(std::floor(x));
            if (ix >= 0 && ix < static_cast<int>(gw)) {
                crossings[static_cast<size_t>(sy) * gw + ix] +=
                    static_cast<float>(e.direction);
            }
        }
    }

    // Sweep per sub-row, accumulate coverage, average for AA
    for (uint32_t y = 0; y < gh; ++y) {
        std::vector<float> alpha(gw, 0.0f);
        for (int s = 0; s < OVERSAMPLE; ++s) {
            uint32_t sy = y * OVERSAMPLE + static_cast<uint32_t>(s);
            float winding = 0.0f;
            for (uint32_t x = 0; x < gw; ++x) {
                winding += crossings[static_cast<size_t>(sy) * gw + x];
                alpha[x] += std::min(std::abs(winding), 1.0f);
            }
        }

        for (uint32_t x = 0; x < gw; ++x) {
            float a = alpha[x] / static_cast<float>(OVERSAMPLE);
            uint8_t av = static_cast<uint8_t>(
                std::min(a, 1.0f) * 255.0f + 0.5f);
            if (av == 0) continue;

            uint32_t ax = dst_x + x;
            uint32_t ay = dst_y + y;
            size_t idx = (static_cast<size_t>(ay) * page_w + ax) * channels;
            if (channels == 1) {
                pixels[idx] = std::max(pixels[idx], av);
            } else {
                for (uint32_t ch = 0; ch < channels; ++ch)
                    pixels[idx + ch] = std::max(pixels[idx + ch], av);
            }
        }
    }
}

} // anonymous namespace

// ============================================================
// Construction
// ============================================================

TextRasterizer::TextRasterizer(const FontLoader& font_loader)
    : font_loader_(font_loader) {}

// ============================================================
// UTF-8 decoding
// ============================================================

std::vector<uint32_t> TextRasterizer::utf8_to_codepoints(const std::string& text) {
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
// Atlas building
// ============================================================

bool TextRasterizer::build_atlas(FontHandle font, CharSet charset) {
    Config default_config;
    return build_atlas(font, charset, default_config);
}

bool TextRasterizer::build_atlas(FontHandle font, CharSet charset,
                                  const Config& config) {
    auto ranges = charset_to_ranges(charset);
    config_ = config;
    current_font_ = font;
    current_charset_ = charset;
    return build_atlas(font, ranges, config);
}

bool TextRasterizer::build_atlas(FontHandle font,
                                  const std::vector<CodepointRange>& ranges) {
    Config default_config;
    return build_atlas(font, ranges, default_config);
}

bool TextRasterizer::build_atlas(FontHandle font,
                                  const std::vector<CodepointRange>& ranges,
                                  const Config& config) {
    if (!font_loader_.is_valid(font)) return false;

    clear();
    config_ = config;
    current_font_ = font;

    // Start first atlas page
    new_page();

    // Rasterize all codepoints in the given ranges
    for (const auto& range : ranges) {
        for (uint32_t cp = range.begin; cp <= range.end; ++cp) {
            if (!font_loader_.has_glyph(font, cp)) continue;
            pack_glyph(font, cp);
        }
    }

    return !glyph_map_.empty();
}

bool TextRasterizer::add_codepoints(FontHandle font,
                                     const std::vector<CodepointRange>& ranges) {
    if (!font_loader_.is_valid(font)) return false;
    if (atlas_pages_.empty()) {
        new_page();
    }

    bool all_ok = true;
    for (const auto& range : ranges) {
        for (uint32_t cp = range.begin; cp <= range.end; ++cp) {
            if (glyph_map_.count(cp)) continue; // Already in atlas
            if (!font_loader_.has_glyph(font, cp)) continue;
            if (!pack_glyph(font, cp)) all_ok = false;
        }
    }
    return all_ok;
}

void TextRasterizer::clear() {
    atlas_pages_.clear();
    glyph_map_.clear();
    shelves_.clear();
    used_pixels_ = 0;
}

// ============================================================
// Glyph query
// ============================================================

const GlyphAtlasEntry* TextRasterizer::get_glyph(uint32_t codepoint) const {
    auto it = glyph_map_.find(codepoint);
    return (it != glyph_map_.end()) ? &it->second : nullptr;
}

bool TextRasterizer::has_glyph(uint32_t codepoint) const {
    return glyph_map_.count(codepoint) > 0;
}

const ImageBuffer* TextRasterizer::get_page(uint32_t page_index) const {
    if (page_index >= atlas_pages_.size()) return nullptr;
    return &atlas_pages_[page_index];
}

// ============================================================
// Shelf-packing algorithm
// ============================================================

void TextRasterizer::new_page() {
    uint32_t channels = config_.subpixel_rendering ? 4 : 1;
    ImageBuffer page;
    page.allocate(config_.atlas_width, config_.atlas_height, channels);
    atlas_pages_.push_back(std::move(page));
    shelves_.clear();
}

bool TextRasterizer::find_shelf_position(uint32_t glyph_w, uint32_t glyph_h,
                                          uint32_t& out_x, uint32_t& out_y) {
    uint32_t padded_w = glyph_w + config_.padding;
    uint32_t padded_h = glyph_h + config_.padding;

    // Try to fit in an existing shelf
    for (auto& shelf : shelves_) {
        if (shelf.cursor_x + padded_w <= config_.atlas_width &&
            padded_h <= shelf.height) {
            out_x = shelf.cursor_x;
            out_y = shelf.y;
            shelf.cursor_x += padded_w;
            return true;
        }
    }

    // Try to fit in a shelf with enough remaining height for glyph
    // (create new shelf)
    uint32_t next_y = 0;
    if (!shelves_.empty()) {
        const auto& last = shelves_.back();
        next_y = last.y + last.height + config_.padding;
    }

    if (next_y + padded_h <= config_.atlas_height) {
        Shelf new_shelf;
        new_shelf.y = next_y;
        new_shelf.height = padded_h;
        new_shelf.cursor_x = padded_w;
        out_x = 0;
        out_y = next_y;
        shelves_.push_back(new_shelf);
        return true;
    }

    return false; // No space in current page
}

bool TextRasterizer::pack_glyph(FontHandle font, uint32_t codepoint) {
    GlyphMetrics metrics;
    if (!font_loader_.get_glyph_metrics(font, codepoint, config_.font_size, metrics)) {
        return false;
    }

    uint32_t gw = static_cast<uint32_t>(std::ceil(metrics.width));
    uint32_t gh = static_cast<uint32_t>(std::ceil(metrics.height));

    // Whitespace characters get a zero-size entry
    if (gw == 0 || gh == 0) {
        GlyphAtlasEntry entry;
        entry.metrics = metrics;
        entry.uv_x0 = 0.0f;
        entry.uv_y0 = 0.0f;
        entry.uv_x1 = 0.0f;
        entry.uv_y1 = 0.0f;
        entry.atlas_page = atlas_pages_.empty()
                           ? 0
                           : static_cast<uint32_t>(atlas_pages_.size()) - 1;
        glyph_map_[codepoint] = entry;
        return true;
    }

    // Clamp to atlas page size
    gw = std::min(gw, config_.atlas_width - config_.padding * 2);
    gh = std::min(gh, config_.atlas_height - config_.padding * 2);

    // Find position in atlas
    uint32_t px, py;
    if (!find_shelf_position(gw, gh, px, py)) {
        // Current page is full — start a new page
        new_page();
        if (!find_shelf_position(gw, gh, px, py)) {
            return false; // Glyph too large for atlas page
        }
    }

    uint32_t page_idx = static_cast<uint32_t>(atlas_pages_.size()) - 1;
    ImageBuffer& page = atlas_pages_[page_idx];

    // Rasterize glyph outline into atlas page at (px, py)
    if (codepoint > ' ') {
        const FontTableEntry* font_entry = font_loader_.get_entry(font);
        const FontMetrics* fm = font_loader_.get_metrics(font);
        if (font_entry && fm && fm->units_per_em > 0) {
            float scale = config_.font_size /
                          static_cast<float>(fm->units_per_em);
            float ascender_px = fm->ascender * scale;
            uint16_t glyph_index =
                font_loader_.codepoint_to_glyph_index(font, codepoint);

            std::vector<RasterEdge> edges;
            if (glyph_index != 0 &&
                extract_glyph_edges(font_entry->raw_data.data(),
                                    font_entry->raw_data.size(),
                                    glyph_index, scale, ascender_px,
                                    edges)) {
                scanline_rasterize(edges, page.pixels.data(),
                                   page.width, page.channels,
                                   px, py, gw, gh);
            }
        }
    }

    used_pixels_ += gw * gh;

    // Build atlas entry
    float inv_w = 1.0f / static_cast<float>(config_.atlas_width);
    float inv_h = 1.0f / static_cast<float>(config_.atlas_height);

    GlyphAtlasEntry entry;
    entry.metrics = metrics;
    entry.uv_x0 = static_cast<float>(px) * inv_w;
    entry.uv_y0 = static_cast<float>(py) * inv_h;
    entry.uv_x1 = static_cast<float>(px + gw) * inv_w;
    entry.uv_y1 = static_cast<float>(py + gh) * inv_h;
    entry.atlas_page = page_idx;

    glyph_map_[codepoint] = entry;
    return true;
}

// ============================================================
// Text vertex generation
// ============================================================

std::vector<TextRasterizer::TextVertex> TextRasterizer::generate_vertices(
    const std::string& text, const TextStyle& style,
    float cursor_x, float cursor_y) const {

    std::vector<TextVertex> vertices;
    auto codepoints = utf8_to_codepoints(text);

    const FontMetrics* fm = font_loader_.get_metrics(current_font_);
    float scale = 1.0f;
    if (fm && fm->units_per_em > 0) {
        scale = style.font_size / config_.font_size; // Ratio of desired to rasterized size
    }

    float ascender = fm ? fm->ascender *
                     (style.font_size / static_cast<float>(fm->units_per_em))
                   : style.font_size * 0.8f;
    float line_h = fm ? fm->line_height *
                   (style.font_size / static_cast<float>(fm->units_per_em)) *
                   style.line_spacing
                 : style.font_size * style.line_spacing;

    float x = cursor_x;
    float y = cursor_y + ascender;

    for (size_t i = 0; i < codepoints.size(); ++i) {
        uint32_t cp = codepoints[i];

        // Newline
        if (cp == '\n') {
            x = cursor_x;
            y += line_h;
            continue;
        }

        const GlyphAtlasEntry* glyph = get_glyph(cp);
        if (!glyph) {
            // Fallback: advance by half font size
            x += style.font_size * 0.5f + style.letter_spacing;
            continue;
        }

        float gw = glyph->metrics.width * scale;
        float gh = glyph->metrics.height * scale;
        float bx = glyph->metrics.bearing_x * scale;
        float by = glyph->metrics.bearing_y * scale;

        // Skip zero-size glyphs (whitespace)
        if (gw > 0 && gh > 0) {
            float x0 = x + bx;
            float y0 = y - by;
            float x1 = x0 + gw;
            float y1 = y0 + gh;

            float u0 = glyph->uv_x0;
            float v0 = glyph->uv_y0;
            float u1 = glyph->uv_x1;
            float v1 = glyph->uv_y1;

            float r = style.color.x;
            float g = style.color.y;
            float b = style.color.z;
            float a = style.color.w;

            // Triangle 1 (top-left, top-right, bottom-left)
            vertices.push_back({x0, y0, u0, v0, r, g, b, a});
            vertices.push_back({x1, y0, u1, v0, r, g, b, a});
            vertices.push_back({x0, y1, u0, v1, r, g, b, a});

            // Triangle 2 (top-right, bottom-right, bottom-left)
            vertices.push_back({x1, y0, u1, v0, r, g, b, a});
            vertices.push_back({x1, y1, u1, v1, r, g, b, a});
            vertices.push_back({x0, y1, u0, v1, r, g, b, a});
        }

        x += glyph->metrics.advance_x * scale + style.letter_spacing;

        // Kerning
        if (i + 1 < codepoints.size() && fm && fm->units_per_em > 0) {
            int16_t kern = font_loader_.get_kerning(
                current_font_, cp, codepoints[i + 1]);
            x += kern * (style.font_size / static_cast<float>(fm->units_per_em));
        }
    }

    return vertices;
}

std::vector<TextRasterizer::TextVertex> TextRasterizer::generate_vertices_ndc(
    const std::string& text, const TextStyle& style,
    float cursor_x, float cursor_y,
    float screen_width, float screen_height) const {

    auto vertices = generate_vertices(text, style, cursor_x, cursor_y);

    // Convert pixel coordinates to NDC [-1, 1]
    float inv_w = 2.0f / screen_width;
    float inv_h = 2.0f / screen_height;

    for (auto& v : vertices) {
        v.pos_x = v.pos_x * inv_w - 1.0f;
        v.pos_y = 1.0f - v.pos_y * inv_h; // Flip Y for OpenGL/Vulkan
    }

    return vertices;
}

// ============================================================
// Stats
// ============================================================

TextRasterizer::Stats TextRasterizer::get_stats() const {
    Stats stats;
    stats.glyph_count  = static_cast<uint32_t>(glyph_map_.size());
    stats.page_count   = static_cast<uint32_t>(atlas_pages_.size());
    stats.atlas_width  = config_.atlas_width;
    stats.atlas_height = config_.atlas_height;
    stats.font_size    = config_.font_size;
    stats.charset      = current_charset_;

    stats.total_bytes = 0;
    for (const auto& page : atlas_pages_) {
        stats.total_bytes += page.byte_size();
    }

    uint32_t total_atlas_pixels = config_.atlas_width * config_.atlas_height *
                                   static_cast<uint32_t>(atlas_pages_.size());
    stats.occupancy = (total_atlas_pixels > 0)
                      ? static_cast<float>(used_pixels_) /
                        static_cast<float>(total_atlas_pixels)
                      : 0.0f;

    return stats;
}

} // namespace pictor
