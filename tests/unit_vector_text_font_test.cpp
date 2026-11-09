/// Pictor::vector_text against a real TrueType font: every printable ASCII
/// glyph of fonts/default.ttf must build, and its mesh must cover the same
/// area as the independent CPU rasterizer (GlyphVectorRasterizer) finds.

#include "pictor/text/font_loader.h"
#include "pictor/text/glyph_vector_rasterizer.h"
#include "pictor/vector_text/glyph_mesh_builder.h"
#include "pictor/vector_text/glyph_mesh_cache.h"
#include "pictor/vector_text/glyph_outline_source.h"
#include "pictor/vector_text/size_tier.h"
#include "test_common.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace pictor;
using namespace pictor_test;

namespace {

double fill_area(const GlyphMeshData& mesh) {
    double area = 0.0;
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const GlyphVertex& a = mesh.vertices[mesh.indices[i]];
        const GlyphVertex& b = mesh.vertices[mesh.indices[i + 1]];
        const GlyphVertex& c = mesh.vertices[mesh.indices[i + 2]];
        const bool fill = a.offset_x == 0 && a.offset_y == 0 && b.offset_x == 0 && b.offset_y == 0 &&
                          c.offset_x == 0 && c.offset_y == 0 && a.coverage == 1 && b.coverage == 1 && c.coverage == 1;
        if (fill) area += std::fabs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) * 0.5;
    }
    return area;
}

}  // namespace

int main() {
    const std::string path = std::string(PICTOR_TEST_SOURCE_DIR) + "/fonts/default.ttf";
    FontLoader fonts;
    const FontHandle font = fonts.load_from_file(path);
    PT_ASSERT(font != INVALID_FONT, "fonts/default.ttf loads");
    if (font == INVALID_FONT) return report("unit_vector_text_font_test");

    const FontOutlineSource source(fonts);
    const GlyphVectorRasterizer rasterizer;
    constexpr float kPx = 128.0f;
    constexpr uint32_t kImage = 3 * 128;
    const float units = source.units_per_em(font);
    PT_ASSERT(units > 0.0f, "units per em");

    int checked = 0;
    for (uint32_t cp = 33; cp < 127; ++cp) {
        const GlyphOutline outline = source.outline(font, cp);
        if (outline.path.empty()) continue;
        const GlyphMeshData mesh = build_glyph_mesh(outline, units, glyph_size_tier(kPx));
        const double mesh_px = fill_area(mesh) * kPx * kPx;

        const ImageBuffer image = rasterizer.render(outline, kImage, kImage, kPx / units, kPx, 2.0f * kPx);
        double raster_px = 0.0;
        for (uint8_t alpha : image.pixels) raster_px += alpha / 255.0;

        // Both are within a fraction of a pixel along the outline; allow 2% plus a few pixels.
        const double tolerance = raster_px * 0.02 + 8.0;
        if (std::fabs(mesh_px - raster_px) > tolerance) {
            std::fprintf(stderr, "       U+%04X mesh %.1f px^2, raster %.1f px^2\n", cp, mesh_px, raster_px);
        }
        PT_ASSERT(std::fabs(mesh_px - raster_px) <= tolerance, "mesh area matches the rasterizer");
        PT_ASSERT(mesh.advance_em > 0.0f, "advance is known");
        ++checked;
    }
    PT_ASSERT(checked > 80, "most printable ASCII glyphs have outlines");

    // A TrueType Collection holding the same font gives the same outlines
    // (the table directory sits behind the 'ttcf' header; offsets are file-relative).
    {
        std::vector<uint8_t> ttf;
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            int c;
            while ((c = std::fgetc(f)) != EOF) ttf.push_back(static_cast<uint8_t>(c));
            std::fclose(f);
        }
        PT_ASSERT(ttf.size() > 12, "font bytes read");
        constexpr uint32_t kHeader = 16;
        std::vector<uint8_t> ttc = {'t', 't', 'c', 'f', 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, kHeader};
        ttc.insert(ttc.end(), ttf.begin(), ttf.end());
        const uint16_t tables = static_cast<uint16_t>((ttc[kHeader + 4] << 8) | ttc[kHeader + 5]);
        for (uint16_t i = 0; i < tables; ++i) {
            const size_t at = kHeader + 12 + size_t(i) * 16 + 8;
            uint32_t offset = (uint32_t(ttc[at]) << 24) | (uint32_t(ttc[at + 1]) << 16) |
                              (uint32_t(ttc[at + 2]) << 8) | ttc[at + 3];
            offset += kHeader;
            ttc[at] = uint8_t(offset >> 24);
            ttc[at + 1] = uint8_t(offset >> 16);
            ttc[at + 2] = uint8_t(offset >> 8);
            ttc[at + 3] = uint8_t(offset);
        }
        const FontHandle collection = fonts.load_from_memory(ttc.data(), ttc.size(), "collection");
        PT_ASSERT(collection != INVALID_FONT, "the .ttc loads");
        if (collection != INVALID_FONT) {
            const GlyphOutline from_ttf = source.outline(font, 'g');
            const GlyphOutline from_ttc = source.outline(collection, 'g');
            PT_ASSERT(!from_ttc.path.empty(), "outline read from the collection");
            PT_ASSERT_OP(from_ttc.path.size(), ==, from_ttf.path.size(), "same outline");
        }
    }

    // The cache serves the same glyphs from one flat array.
    GlyphMeshCache cache(source);
    for (uint32_t cp = 33; cp < 127; ++cp) (void)cache.mesh_for(font, cp, glyph_size_tier(16.0f));
    PT_ASSERT_OP(cache.mesh_count(), ==, 94, "one mesh per codepoint");
    PT_ASSERT(cache.indices().size() % 3 == 0, "whole triangles");
    return report("unit_vector_text_font_test");
}
