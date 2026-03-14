#include "pictor/text/text_rasterizer.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace pictor {

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
        entry.atlas_page = static_cast<uint32_t>(atlas_pages_.size()) - 1;
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

    // Rasterize glyph into atlas page at (px, py)
    // Simple box rasterization (same approach as TextImageRenderer).
    // A production implementation would use actual outline rasterization.
    if (codepoint > ' ') {
        uint32_t channels = page.channels;
        for (uint32_t gy = 0; gy < gh; ++gy) {
            for (uint32_t gx = 0; gx < gw; ++gx) {
                uint32_t ax = px + gx;
                uint32_t ay = py + gy;
                if (ax >= page.width || ay >= page.height) continue;

                size_t idx = (static_cast<size_t>(ay) * page.width + ax) * channels;
                if (channels == 1) {
                    page.pixels[idx] = 255;
                } else {
                    page.pixels[idx + 0] = 255;
                    page.pixels[idx + 1] = 255;
                    page.pixels[idx + 2] = 255;
                    page.pixels[idx + 3] = 255;
                }
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
