#pragma once

#include "pictor/text/text_types.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace pictor {

/// OTF/TTF font file loader and glyph metrics provider.
///
/// Implements a minimal TrueType/OpenType parser that reads:
///   - 'head' table (units-per-em, index-to-loc format)
///   - 'hhea' / 'OS/2' tables (ascender, descender, line gap)
///   - 'maxp' table (num glyphs)
///   - 'cmap' table (Unicode → glyph index mapping, format 4 + format 12)
///   - 'hmtx' table (advance widths, left side bearings)
///   - 'loca' / 'glyf' tables (glyph outlines — TrueType)
///   - 'CFF ' / 'CFF2' tables (PostScript outlines — OpenType CFF, detection only)
///
/// This is the foundation layer for all text rendering approaches.
/// It does NOT depend on FreeType or any external font library.
class FontLoader {
public:
    FontLoader() = default;
    ~FontLoader() = default;

    FontLoader(const FontLoader&) = delete;
    FontLoader& operator=(const FontLoader&) = delete;

    // ---- Loading ----

    /// Load a font from a file path (TTF or OTF).
    /// Returns a FontHandle, or INVALID_FONT on failure.
    FontHandle load_from_file(const std::string& path);

    /// Load a font from raw memory (caller retains ownership of data
    /// until this call returns; internal copy is made).
    FontHandle load_from_memory(const void* data, size_t size,
                                 const std::string& name = "memory_font");

    /// Unload a font and free associated data.
    void unload(FontHandle handle);

    // ---- Query ----

    /// Check if a handle is valid
    bool is_valid(FontHandle handle) const;

    /// Get font-level metrics
    const FontMetrics* get_metrics(FontHandle handle) const;

    /// Get font family name
    const std::string* get_family_name(FontHandle handle) const;

    /// Get glyph metrics for a specific codepoint at a given pixel size.
    /// Returns false if the glyph is not available.
    bool get_glyph_metrics(FontHandle handle, uint32_t codepoint,
                           float font_size, GlyphMetrics& out) const;

    /// Map a codepoint to a glyph index (0 = .notdef / missing glyph).
    uint16_t codepoint_to_glyph_index(FontHandle handle, uint32_t codepoint) const;

    /// Check if a font contains a glyph for the given codepoint.
    bool has_glyph(FontHandle handle, uint32_t codepoint) const;

    /// Get the raw font data (for passing to rasterizer / SVG parser).
    const FontTableEntry* get_entry(FontHandle handle) const;

    /// Get kerning adjustment between two glyphs (in font units).
    /// Returns 0 if no kerning data is available.
    int16_t get_kerning(FontHandle handle, uint32_t left_codepoint,
                        uint32_t right_codepoint) const;

    /// Get all loaded font handles
    std::vector<FontHandle> all_handles() const;

    /// Total number of loaded fonts
    uint32_t count() const { return static_cast<uint32_t>(fonts_.size()); }

private:
    /// Internal: parse TrueType/OpenType tables from raw data
    bool parse_font_tables(FontTableEntry& entry);

    /// Internal: parse 'cmap' table and build codepoint mapping
    bool parse_cmap(const uint8_t* data, size_t size, FontTableEntry& entry);

    /// Internal: parse 'head' table
    bool parse_head(const uint8_t* data, size_t size, FontTableEntry& entry);

    /// Internal: parse 'hhea' + 'hmtx' tables
    bool parse_horizontal_metrics(const uint8_t* data, size_t size,
                                   FontTableEntry& entry);

    /// Internal: locate a table in the font file
    struct TableRecord {
        uint32_t tag    = 0;
        uint32_t offset = 0;
        uint32_t length = 0;
    };

    std::vector<TableRecord> read_table_directory(const uint8_t* data, size_t size);

    // Per-font parsed data (indexed by handle)
    struct ParsedFontData {
        uint16_t num_glyphs          = 0;
        uint16_t num_h_metrics       = 0;
        int16_t  index_to_loc_format = 0;
        bool     is_cff              = false;
        std::unordered_map<uint32_t, uint16_t> cmap;  // codepoint → glyph index
        std::vector<uint16_t> advance_widths;          // per glyph index
        std::vector<int16_t>  left_side_bearings;
        // Kerning
        std::unordered_map<uint64_t, int16_t> kern_map; // (left<<16|right) → adjustment
        // Table offsets within raw_data for glyf/loca access
        uint32_t glyf_offset = 0;
        uint32_t glyf_length = 0;
        uint32_t loca_offset = 0;
        uint32_t loca_length = 0;
    };

    std::vector<FontTableEntry> fonts_;
    std::vector<ParsedFontData> parsed_;
    FontHandle next_handle_ = 0;
};

} // namespace pictor
