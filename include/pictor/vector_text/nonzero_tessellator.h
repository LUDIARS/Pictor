#pragma once

#include "pictor/vector_text/glyph_contours.h"
#include "pictor/vector_text/glyph_mesh.h"

namespace pictor {

/// Fills the nonzero-winding interior of the contours with triangles.
///
/// The plane is cut into horizontal slabs at every vertex and edge crossing, so
/// edges never cross inside a slab; each run of nonzero winding becomes a
/// trapezoid. Holes, overlapping components and self-intersections follow the
/// nonzero rule. Appends fill vertices (coverage 1) and indices to `out`.
void tessellate_nonzero(const GlyphContours& contours, GlyphMeshData& out);

} // namespace pictor
