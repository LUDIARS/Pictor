#include "pictor/vector_text/glyph_mesh_builder.h"

#include "pictor/vector_text/glyph_contours.h"
#include "pictor/vector_text/glyph_feather.h"
#include "pictor/vector_text/nonzero_tessellator.h"
#include "pictor/vector_text/size_tier.h"

#include <stdexcept>

namespace pictor {

GlyphMeshData build_glyph_mesh(const GlyphOutline& outline, float units_per_em, uint32_t tier) {
    GlyphOutline source = outline;
    if (!(source.em_size > 0.0f)) source.em_size = units_per_em;
    if (!(source.em_size > 0.0f)) throw std::invalid_argument("vector_text: font has no em size");
    GlyphMeshData mesh;
    mesh.advance_em = outline.advance_x / source.em_size;
    const GlyphContours contours = flatten_glyph_contours(source, glyph_tier_tolerance_em(tier));
    tessellate_nonzero(contours, mesh);
    add_feather_band(contours, mesh);
    return mesh;
}

}  // namespace pictor
