#pragma once

#include <cstdint>
#include <vector>

namespace pictor {

/// One vertex of a glyph mesh, in em units (1.0 = one em), y-up.
///
/// Fill vertices have `offset = 0` and `coverage = 1`. Feather vertices sit on
/// the outline with `offset` pointing away from the fill (unit length in em
/// space); the vertex shader moves them by the feather width in screen pixels
/// and fades `coverage` to 0, so the 1px edge stays 1px at any size or rotation.
struct GlyphVertex {
    float x = 0.0f, y = 0.0f;
    float offset_x = 0.0f, offset_y = 0.0f;
    float coverage = 1.0f;
};
static_assert(sizeof(GlyphVertex) == 20, "GlyphVertex is uploaded as-is");

/// Triangles of one glyph at one size tier (indices are local to `vertices`).
struct GlyphMeshData {
    std::vector<GlyphVertex> vertices;
    std::vector<uint32_t>    indices;
    float advance_em = 0.0f;
};

/// Where one cached glyph lives inside the cache's flat vertex/index arrays.
struct GlyphMeshRange {
    uint32_t first_index  = 0;
    uint32_t index_count  = 0;  // 0 for blank glyphs (space)
    uint32_t first_vertex = 0;
    float    advance_em   = 0.0f;
};

} // namespace pictor
