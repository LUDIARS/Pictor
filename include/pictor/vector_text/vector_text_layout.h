#pragma once

#include "pictor/vector_text/glyph_mesh_cache.h"
#include "pictor/vector_text/vector_text_batch.h"

#include <string_view>

namespace pictor {

struct VectorTextStyle {
    float pixels_per_em = 16.0f;
    float color[4]      = {1, 1, 1, 1};
    float feather_px    = 1.0f;
    float embolden_px   = 0.0f;  // synthetic bold: the outline pushed out by this many pixels
    float letter_spacing_px = 0.0f;  // added after every glyph's advance
    float rotation_rad  = 0.0f;  // about the line origin, clockwise on screen
};

/// Lays out UTF-8 text on a screen-space baseline (y down) and appends one
/// instance per visible glyph. '\n' starts a new line. Invalid UTF-8 bytes
/// become U+FFFD. Returns the advance width of the longest line in pixels.
float layout_vector_text(GlyphMeshCache& cache, FontHandle font, std::string_view utf8,
                         const VectorTextStyle& style, float origin_x, float baseline_y,
                         VectorTextBatch& batch);

/// Width in pixels of the longest line, without adding anything to a batch.
float measure_vector_text(GlyphMeshCache& cache, FontHandle font, std::string_view utf8,
                          float pixels_per_em, float letter_spacing_px = 0.0f);

/// Next codepoint of UTF-8 text; advances `at`. Invalid bytes yield U+FFFD.
uint32_t next_utf8_codepoint(std::string_view utf8, size_t& at);

} // namespace pictor
