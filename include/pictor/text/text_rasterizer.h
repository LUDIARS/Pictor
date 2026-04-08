#pragma once

#include "pictor/text/text_types.h"
#include "pictor/text/font_loader.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace pictor {

/// Rasterized font texture atlas builder and fast text renderer.
///
/// Pre-rasterizes glyphs for a specified character set into one or more
/// texture atlas pages. The atlas can then be uploaded to the GPU for
/// high-performance text rendering using textured quads.
///
/// Architecture:
///   1. **Build phase**: Rasterize selected character set glyphs into
///      alpha-channel atlas pages using a shelf-packing algorithm.
///   2. **Query phase**: Look up GlyphAtlasEntry for any codepoint to
///      get UV coordinates and metrics for quad generation.
///   3. **Render phase**: Generate vertex data (position + UV quads)
///      for a text string, ready for GPU instanced/batched draw.
///
/// Character set selection allows memory/quality trade-offs:
///   - ASCII only: ~95 glyphs, tiny atlas (256x256 is enough)
///   - Western: ~400 glyphs, small atlas (512x512)
///   - Japanese: ~21,000+ glyphs, large atlas (multiple 2048x2048 pages)
///   - Full Unicode: all available glyphs
class TextRasterizer {
public:
    explicit TextRasterizer(const FontLoader& font_loader);
    ~TextRasterizer() = default;

    TextRasterizer(const TextRasterizer&) = delete;
    TextRasterizer& operator=(const TextRasterizer&) = delete;

    // ---- Configuration ----

    struct Config {
        uint32_t atlas_width      = 1024;    // Atlas page width in pixels
        uint32_t atlas_height     = 1024;    // Atlas page height in pixels
        float    font_size        = 32.0f;   // Rasterization size in pixels
        uint32_t padding          = 2;       // Padding between glyphs (pixels)
        bool     generate_sdf     = false;   // Generate SDF instead of bitmap
        float    sdf_spread       = 4.0f;    // SDF distance spread in pixels
        bool     subpixel_rendering = false; // LCD subpixel rendering (3x width)
    };

    // ---- Atlas building ----

    /// Build a glyph atlas for the specified font and character set.
    /// Returns true on success. Atlas pages are stored internally.
    bool build_atlas(FontHandle font, CharSet charset);
    bool build_atlas(FontHandle font, CharSet charset,
                     const Config& config);

    /// Build an atlas from explicit codepoint ranges.
    bool build_atlas(FontHandle font,
                     const std::vector<CodepointRange>& ranges);
    bool build_atlas(FontHandle font,
                     const std::vector<CodepointRange>& ranges,
                     const Config& config);

    /// Add additional codepoints to an existing atlas (incremental build).
    /// Returns true if all codepoints were added successfully.
    bool add_codepoints(FontHandle font,
                        const std::vector<CodepointRange>& ranges);

    /// Clear atlas data and free memory.
    void clear();

    // ---- Glyph query ----

    /// Look up a glyph in the atlas. Returns nullptr if not found.
    const GlyphAtlasEntry* get_glyph(uint32_t codepoint) const;

    /// Check if a codepoint is in the atlas.
    bool has_glyph(uint32_t codepoint) const;

    /// Get all atlas entries
    const std::unordered_map<uint32_t, GlyphAtlasEntry>& all_glyphs() const {
        return glyph_map_;
    }

    // ---- Atlas page access ----

    /// Get the number of atlas pages
    uint32_t page_count() const { return static_cast<uint32_t>(atlas_pages_.size()); }

    /// Get a specific atlas page image (alpha-channel, or RGBA for subpixel).
    const ImageBuffer* get_page(uint32_t page_index) const;

    /// Get atlas page dimensions
    uint32_t atlas_width() const { return config_.atlas_width; }
    uint32_t atlas_height() const { return config_.atlas_height; }

    // ---- Text vertex generation ----

    /// A single quad vertex for text rendering
    struct TextVertex {
        float pos_x, pos_y;   // Screen-space position
        float uv_x,  uv_y;   // Atlas UV
        float r, g, b, a;     // Color
    };

    /// Generate quad vertices for a text string.
    /// Returns a list of vertices (6 per glyph: 2 triangles).
    /// The caller uploads these to a GPU vertex buffer.
    std::vector<TextVertex> generate_vertices(
        const std::string& text,
        const TextStyle& style = {},
        float cursor_x = 0.0f,
        float cursor_y = 0.0f) const;

    /// Generate vertices with screen-space transform.
    /// screen_width/height are used to convert pixel coords to NDC [-1,1].
    std::vector<TextVertex> generate_vertices_ndc(
        const std::string& text,
        const TextStyle& style,
        float cursor_x, float cursor_y,
        float screen_width, float screen_height) const;

    // ---- Stats ----

    struct Stats {
        uint32_t glyph_count     = 0;   // Total rasterized glyphs
        uint32_t page_count      = 0;   // Number of atlas pages
        size_t   total_bytes     = 0;   // Total atlas memory usage
        float    occupancy       = 0.0f; // Atlas space utilization (0..1)
        uint32_t atlas_width     = 0;
        uint32_t atlas_height    = 0;
        float    font_size       = 0.0f;
        CharSet  charset         = CharSet::ASCII;
    };

    Stats get_stats() const;

private:
    /// Internal: rasterize a single glyph and pack into the current atlas page.
    bool pack_glyph(FontHandle font, uint32_t codepoint);

    /// Internal: start a new atlas page.
    void new_page();

    /// Internal: try to find space in the current shelf for a glyph of given size.
    /// Returns true and sets out_x/out_y if space is found.
    bool find_shelf_position(uint32_t glyph_w, uint32_t glyph_h,
                             uint32_t& out_x, uint32_t& out_y);

    /// Decode UTF-8 string to codepoints
    static std::vector<uint32_t> utf8_to_codepoints(const std::string& text);

    const FontLoader& font_loader_;
    Config config_;
    FontHandle current_font_ = INVALID_FONT;
    CharSet current_charset_ = CharSet::ASCII;

    // Atlas pages
    std::vector<ImageBuffer> atlas_pages_;

    // Glyph lookup
    std::unordered_map<uint32_t, GlyphAtlasEntry> glyph_map_;

    // Shelf-packing state (per current page)
    struct Shelf {
        uint32_t y      = 0;  // Top-left Y of this shelf
        uint32_t height = 0;  // Shelf height (tallest glyph in shelf)
        uint32_t cursor_x = 0; // Next available X position
    };
    std::vector<Shelf> shelves_;
    uint32_t used_pixels_ = 0; // For occupancy calculation
};

} // namespace pictor
