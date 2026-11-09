#pragma once

#include "pictor/text/text_types.h"
#include "pictor/vector_text/glyph_mesh.h"

#include <cstdint>

namespace pictor {

/// Outline (font units) -> fill triangles + feather band in em units, flattened
/// at the tolerance of `tier`. `units_per_em` is used when the outline does not
/// carry its own em size.
GlyphMeshData build_glyph_mesh(const GlyphOutline& outline, float units_per_em, uint32_t tier);

} // namespace pictor
