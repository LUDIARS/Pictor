#pragma once

#include "pictor/core/types.h"
#include <cstdint>
#include <string>
#include <vector>
#include <cstring>

namespace pictor {

// ============================================================
// Font Handle
// ============================================================

using FontHandle = uint32_t;
constexpr FontHandle INVALID_FONT = std::numeric_limits<uint32_t>::max();

// ============================================================
// Character Set — selectable glyph ranges for rasterization
// ============================================================

enum class CharSet : uint32_t {
    ASCII            = 1 << 0,   // U+0020..U+007E (95 chars)
    LATIN_EXTENDED   = 1 << 1,   // U+00A0..U+024F (Latin-1 Supp + Extended-A/B)
    CYRILLIC         = 1 << 2,   // U+0400..U+04FF
    GREEK            = 1 << 3,   // U+0370..U+03FF
    CJK_COMMON       = 1 << 4,   // U+4E00..U+9FFF (CJK Unified Ideographs)
    HIRAGANA         = 1 << 5,   // U+3040..U+309F
    KATAKANA         = 1 << 6,   // U+30A0..U+30FF
    HANGUL           = 1 << 7,   // U+AC00..U+D7AF
    ARABIC           = 1 << 8,   // U+0600..U+06FF
    DEVANAGARI       = 1 << 9,   // U+0900..U+097F
    SYMBOLS          = 1 << 10,  // Box drawing, math, arrows, emoji subset
    FULL_UNICODE     = 1u << 31,  // All available glyphs in the font

    // Convenience combinations
    JAPANESE         = static_cast<uint32_t>(HIRAGANA) |
                       static_cast<uint32_t>(KATAKANA) |
                       static_cast<uint32_t>(CJK_COMMON) |
                       static_cast<uint32_t>(ASCII),
    KOREAN           = static_cast<uint32_t>(HANGUL) |
                       static_cast<uint32_t>(ASCII),
    WESTERN          = static_cast<uint32_t>(ASCII) |
                       static_cast<uint32_t>(LATIN_EXTENDED),
};

inline CharSet operator|(CharSet a, CharSet b) {
    return static_cast<CharSet>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline CharSet operator&(CharSet a, CharSet b) {
    return static_cast<CharSet>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline bool has_charset(CharSet set, CharSet flag) {
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(flag)) != 0;
}

/// A Unicode codepoint range (inclusive)
struct CodepointRange {
    uint32_t begin = 0;
    uint32_t end   = 0;
};

/// Returns codepoint ranges for a given CharSet
inline std::vector<CodepointRange> charset_to_ranges(CharSet cs) {
    std::vector<CodepointRange> ranges;

    if (has_charset(cs, CharSet::FULL_UNICODE)) {
        ranges.push_back({0x0020, 0xFFFF});
        return ranges;
    }
    if (has_charset(cs, CharSet::ASCII))          ranges.push_back({0x0020, 0x007E});
    if (has_charset(cs, CharSet::LATIN_EXTENDED)) ranges.push_back({0x00A0, 0x024F});
    if (has_charset(cs, CharSet::CYRILLIC))       ranges.push_back({0x0400, 0x04FF});
    if (has_charset(cs, CharSet::GREEK))          ranges.push_back({0x0370, 0x03FF});
    if (has_charset(cs, CharSet::CJK_COMMON))     ranges.push_back({0x4E00, 0x9FFF});
    if (has_charset(cs, CharSet::HIRAGANA))       ranges.push_back({0x3040, 0x309F});
    if (has_charset(cs, CharSet::KATAKANA))       ranges.push_back({0x30A0, 0x30FF});
    if (has_charset(cs, CharSet::HANGUL))         ranges.push_back({0xAC00, 0xD7AF});
    if (has_charset(cs, CharSet::ARABIC))         ranges.push_back({0x0600, 0x06FF});
    if (has_charset(cs, CharSet::DEVANAGARI))     ranges.push_back({0x0900, 0x097F});
    if (has_charset(cs, CharSet::SYMBOLS))        ranges.push_back({0x2190, 0x27BF}); // Arrows+MathOps+Misc

    return ranges;
}

// ============================================================
// Glyph Metrics
// ============================================================

/// Metrics for a single glyph
struct GlyphMetrics {
    uint32_t codepoint   = 0;
    float    advance_x   = 0.0f;   // Horizontal advance (pixels at rasterized size)
    float    bearing_x   = 0.0f;   // Offset from cursor to left edge
    float    bearing_y   = 0.0f;   // Offset from baseline to top edge
    float    width       = 0.0f;   // Glyph bitmap width
    float    height      = 0.0f;   // Glyph bitmap height
};

/// Metrics for a single glyph within a texture atlas
struct GlyphAtlasEntry {
    GlyphMetrics metrics;
    float        uv_x0 = 0.0f;   // Atlas UV left
    float        uv_y0 = 0.0f;   // Atlas UV top
    float        uv_x1 = 0.0f;   // Atlas UV right
    float        uv_y1 = 0.0f;   // Atlas UV bottom
    uint32_t     atlas_page = 0;  // Atlas page index (for multi-page atlases)
};

// ============================================================
// Font Metrics (per-font global metrics)
// ============================================================

struct FontMetrics {
    float ascender       = 0.0f;   // Ascender line (above baseline)
    float descender      = 0.0f;   // Descender line (below baseline, typically negative)
    float line_height    = 0.0f;   // Recommended line spacing
    float max_advance    = 0.0f;   // Maximum glyph advance width
    uint32_t units_per_em = 0;     // Font design units per em
};

// ============================================================
// Text Alignment
// ============================================================

enum class TextAlignH : uint8_t {
    LEFT   = 0,
    CENTER = 1,
    RIGHT  = 2
};

enum class TextAlignV : uint8_t {
    TOP      = 0,
    MIDDLE   = 1,
    BOTTOM   = 2,
    BASELINE = 3
};

// ============================================================
// Text Style
// ============================================================

struct TextStyle {
    float    font_size    = 16.0f;    // Font size in pixels
    float4   color        = {1.0f, 1.0f, 1.0f, 1.0f};
    TextAlignH align_h    = TextAlignH::LEFT;
    TextAlignV align_v    = TextAlignV::BASELINE;
    float    line_spacing  = 1.2f;    // Multiplier on font line height
    float    letter_spacing = 0.0f;   // Additional spacing between characters (px)
    float    max_width     = 0.0f;    // 0 = no line wrapping
    bool     word_wrap     = true;
};

// ============================================================
// Font Table Entry (internal)
// ============================================================

struct FontTableEntry {
    FontHandle handle       = INVALID_FONT;
    std::string family_name;
    std::string style_name;             // "Regular", "Bold", "Italic", etc.
    std::vector<uint8_t> raw_data;      // Raw font file data (TTF/OTF)
    FontMetrics metrics;
    bool        loaded = false;
};

// ============================================================
// SVG Path — minimal representation for glyph outlines
// ============================================================

enum class SvgPathCommand : uint8_t {
    MOVE_TO    = 0,   // M x,y
    LINE_TO    = 1,   // L x,y
    QUAD_TO    = 2,   // Q cx,cy x,y
    CUBIC_TO   = 3,   // C c1x,c1y c2x,c2y x,y
    CLOSE      = 4    // Z
};

struct SvgPathPoint {
    SvgPathCommand command;
    float x  = 0.0f, y  = 0.0f;   // End point
    float cx = 0.0f, cy = 0.0f;   // Control point 1
    float cx2 = 0.0f, cy2 = 0.0f; // Control point 2 (cubic only)
};

/// A single glyph outline as SVG-compatible path data
struct GlyphOutline {
    uint32_t codepoint = 0;
    std::vector<SvgPathPoint> path;
    float advance_x = 0.0f;
    float em_size    = 0.0f;     // Font units per em
};

// ============================================================
// Rasterized Image Buffer
// ============================================================

struct ImageBuffer {
    std::vector<uint8_t> pixels;   // Row-major, top-to-bottom
    uint32_t width  = 0;
    uint32_t height = 0;
    uint32_t channels = 1;         // 1=alpha, 4=RGBA

    size_t byte_size() const { return static_cast<size_t>(width) * height * channels; }

    void allocate(uint32_t w, uint32_t h, uint32_t ch) {
        width = w; height = h; channels = ch;
        pixels.resize(byte_size(), 0);
    }

    void clear() {
        std::memset(pixels.data(), 0, pixels.size());
    }
};

} // namespace pictor
