#include "pictor/text/text_image_renderer.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace pictor {

// ============================================================
// TrueType outline parsing and scanline rasterization
// (Shared logic with TextRasterizer — extracted into both
//  translation units to avoid cross-component coupling.)
// ============================================================

namespace {

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

struct RasterEdge {
    float x0, y0, x1, y1;
    int direction;
};

void add_edge(std::vector<RasterEdge>& edges,
              float x0, float y0, float x1, float y1) {
    if (std::abs(y1 - y0) < 0.001f) return;
    if (y0 < y1) edges.push_back({x0, y0, x1, y1, 1});
    else          edges.push_back({x1, y1, x0, y0, -1});
}

void flatten_quad_bezier(std::vector<RasterEdge>& edges,
                         float x0, float y0, float cx, float cy,
                         float x1, float y1, float tol = 0.25f) {
    float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
    float dx = cx - mx, dy = cy - my;
    if (dx*dx + dy*dy <= tol*tol) { add_edge(edges, x0, y0, x1, y1); return; }
    float x01=(x0+cx)*0.5f, y01=(y0+cy)*0.5f;
    float x12=(cx+x1)*0.5f, y12=(cy+y1)*0.5f;
    float xm=(x01+x12)*0.5f, ym=(y01+y12)*0.5f;
    flatten_quad_bezier(edges, x0,y0, x01,y01, xm,ym, tol);
    flatten_quad_bezier(edges, xm,ym, x12,y12, x1,y1, tol);
}

bool extract_glyph_edges(const uint8_t* data, size_t data_size,
                          uint16_t glyph_index,
                          float scale, float ascender_px,
                          std::vector<RasterEdge>& out) {
    if (data_size < 12) return false;
    uint16_t num_tables = rd_u16(data + 4);
    uint32_t glyf_off = 0, loca_off = 0;
    uint16_t num_glyphs = 0; int16_t loc_fmt = 0;
    for (uint16_t t = 0; t < num_tables; ++t) {
        size_t r = 12 + t * 16;
        if (r + 16 > data_size) break;
        uint32_t tag = rd_u32(data+r), off = rd_u32(data+r+8);
        if (tag == mk_tag('g','l','y','f'))                              glyf_off = off;
        else if (tag == mk_tag('l','o','c','a'))                         loca_off = off;
        else if (tag == mk_tag('m','a','x','p') && off+6 <= data_size)   num_glyphs = rd_u16(data+off+4);
        else if (tag == mk_tag('h','e','a','d') && off+54 <= data_size)  loc_fmt = rd_i16(data+off+50);
    }
    if (!glyf_off || !loca_off || glyph_index >= num_glyphs) return false;
    uint32_t g_off, g_end;
    if (loc_fmt == 0) {
        uint32_t li = loca_off + glyph_index*2;
        if (li+4 > data_size) return false;
        g_off = static_cast<uint32_t>(rd_u16(data+li)) * 2;
        g_end = static_cast<uint32_t>(rd_u16(data+li+2)) * 2;
    } else {
        uint32_t li = loca_off + glyph_index*4;
        if (li+8 > data_size) return false;
        g_off = rd_u32(data+li); g_end = rd_u32(data+li+4);
    }
    if (g_off == g_end) return false;
    uint32_t abs_ = glyf_off + g_off;
    if (abs_+10 > data_size) return false;
    const uint8_t* g = data + abs_;
    int16_t nc = rd_i16(g);
    if (nc <= 0) return false;
    uint32_t p = 10;
    std::vector<uint16_t> ends(nc);
    for (int16_t c = 0; c < nc; ++c) {
        if (abs_+p+2 > data_size) return false;
        ends[c] = rd_u16(g+p); p += 2;
    }
    uint16_t total = ends.back()+1;
    if (abs_+p+2 > data_size) return false;
    p += 2 + rd_u16(g+p);
    std::vector<uint8_t> fl(total);
    for (uint16_t i = 0; i < total;) {
        if (abs_+p >= data_size) return false;
        uint8_t f = g[p++]; fl[i++] = f;
        if (f & 0x08) { if (abs_+p >= data_size) return false;
            uint8_t rep = g[p++];
            for (uint8_t rr = 0; rr < rep && i < total; ++rr) fl[i++] = f;
        }
    }
    std::vector<int16_t> xs(total,0); int16_t xv=0;
    for (uint16_t i=0;i<total;++i) {
        if (fl[i]&0x02) { if(abs_+p>=data_size) return false; uint8_t d=g[p++];
            xv += (fl[i]&0x10)?d:-static_cast<int16_t>(d);
        } else if (!(fl[i]&0x10)) { if(abs_+p+1>=data_size) return false;
            xv += rd_i16(g+p); p+=2; }
        xs[i]=xv;
    }
    std::vector<int16_t> ys(total,0); int16_t yv_=0;
    for (uint16_t i=0;i<total;++i) {
        if (fl[i]&0x04) { if(abs_+p>=data_size) return false; uint8_t d=g[p++];
            yv_ += (fl[i]&0x20)?d:-static_cast<int16_t>(d);
        } else if (!(fl[i]&0x20)) { if(abs_+p+1>=data_size) return false;
            yv_ += rd_i16(g+p); p+=2; }
        ys[i]=yv_;
    }
    auto tx=[&](float fx)->float{ return fx*scale; };
    auto ty=[&](float fy)->float{ return ascender_px - fy*scale; };
    uint16_t cs=0;
    for (int16_t c=0; c<nc; ++c) {
        uint16_t ce = ends[c]; if (ce<cs) break;
        bool fon = (fl[cs]&1)!=0; float sx,sy;
        if (fon) { sx=tx(xs[cs]); sy=ty(ys[cs]); }
        else { bool lon=(fl[ce]&1)!=0;
            if (lon) { sx=tx(xs[ce]); sy=ty(ys[ce]); }
            else { sx=tx((xs[cs]+xs[ce])*0.5f); sy=ty((ys[cs]+ys[ce])*0.5f); } }
        float cx_=sx, cy_=sy;
        for (uint16_t i=cs; i<=ce; ++i) {
            uint16_t nxt=(i<ce)?i+1:cs;
            bool co=(fl[i]&1)!=0, no=(fl[nxt]&1)!=0;
            if (co&&no) { float nx=tx(xs[nxt]),ny=ty(ys[nxt]);
                add_edge(out,cx_,cy_,nx,ny); cx_=nx; cy_=ny; }
            else if (co&&!no) { uint16_t nn=(nxt<ce)?nxt+1:cs;
                bool nno=(fl[nn]&1)!=0; float cpx=tx(xs[nxt]),cpy=ty(ys[nxt]); float ex,ey;
                if (nno) { ex=tx(xs[nn]); ey=ty(ys[nn]); i=nxt; }
                else { ex=(cpx+tx(xs[nn]))*0.5f; ey=(cpy+ty(ys[nn]))*0.5f; i=nxt; }
                flatten_quad_bezier(out,cx_,cy_,cpx,cpy,ex,ey); cx_=ex; cy_=ey; }
        }
        add_edge(out,cx_,cy_,sx,sy); cs=ce+1;
    }
    return !out.empty();
}

// Rasterize edges into a standalone ImageBuffer (1-channel alpha)
void scanline_rasterize_buffer(const std::vector<RasterEdge>& edges,
                               uint8_t* pixels, uint32_t w, uint32_t h) {
    if (!w || !h || edges.empty()) return;
    constexpr int OV = 4;
    uint32_t sh = h * OV;
    std::vector<float> cross(static_cast<size_t>(w)*sh, 0.0f);
    for (const auto& e : edges) {
        int s0 = std::max(0, static_cast<int>(std::ceil(e.y0*OV)));
        int s1 = std::min(static_cast<int>(sh), static_cast<int>(std::ceil(e.y1*OV)));
        float dy = e.y1-e.y0; if (std::abs(dy)<0.001f) continue;
        float inv = 1.0f/dy;
        for (int sy=s0; sy<s1; ++sy) {
            float y = (sy+0.5f)/static_cast<float>(OV);
            float x = e.x0 + (e.x1-e.x0)*(y-e.y0)*inv;
            int ix = static_cast<int>(std::floor(x));
            if (ix>=0 && ix<static_cast<int>(w))
                cross[static_cast<size_t>(sy)*w+ix] += static_cast<float>(e.direction);
        }
    }
    for (uint32_t y=0; y<h; ++y) {
        std::vector<float> alpha(w, 0.0f);
        for (int s=0; s<OV; ++s) {
            uint32_t sy = y*OV+s; float wn=0.0f;
            for (uint32_t x=0; x<w; ++x) {
                wn += cross[static_cast<size_t>(sy)*w+x];
                alpha[x] += std::min(std::abs(wn), 1.0f);
            }
        }
        for (uint32_t x=0; x<w; ++x) {
            float a = std::min(alpha[x]/static_cast<float>(OV), 1.0f);
            pixels[y*w+x] = static_cast<uint8_t>(a*255.0f+0.5f);
        }
    }
}

} // anonymous namespace

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

    // Rasterize actual glyph outlines from the TrueType 'glyf' table
    const FontTableEntry* entry = font_loader_.get_entry(font);
    if (!entry) return result;

    if (codepoint > ' ') {
        const FontMetrics* fm = font_loader_.get_metrics(font);
        if (fm && fm->units_per_em > 0) {
            float scale = font_size / static_cast<float>(fm->units_per_em);
            float ascender_px = fm->ascender * scale;
            uint16_t glyph_index =
                font_loader_.codepoint_to_glyph_index(font, codepoint);

            std::vector<RasterEdge> edges;
            if (glyph_index != 0 &&
                extract_glyph_edges(entry->raw_data.data(),
                                    entry->raw_data.size(),
                                    glyph_index, scale, ascender_px,
                                    edges)) {
                scanline_rasterize_buffer(edges, result.pixels.data(), w, h);
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
