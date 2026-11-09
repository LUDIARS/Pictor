#pragma once

#include "pictor/vector_text/glyph_contours.h"
#include "pictor/vector_text/glyph_mesh.h"

namespace pictor {

/// Adds the anti-aliasing band outside the filled shape.
///
/// Only edges with fill on exactly one side get a band (edges buried inside an
/// overlap get none). Corners between banded edges get a wedge so the band has
/// no gaps. Appends feather vertices and indices to `out`.
void add_feather_band(const GlyphContours& contours, GlyphMeshData& out);

} // namespace pictor
