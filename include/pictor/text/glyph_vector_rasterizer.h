#pragma once
#include "pictor/text/text_types.h"

namespace pictor {
/// Converts reusable, y-up font-unit vector paths into an alpha image.
/// Curves remain vectors until this final size-dependent operation. Nonzero
/// winding preserves holes and overlapping components. No OS font API required.
class GlyphVectorRasterizer {
public:
    /// pixel_x = path_x * scale + origin_x;
    /// pixel_y = baseline_y - path_y * scale.
    /// Accepts MOVE/LINE/QUAD/CUBIC/CLOSE, including glyph_path_effects output.
    /// Clips coverage to the requested image. Size is bounded to 4096 per axis.
    /// Invalid/non-finite paths, excessive complexity or size throw invalid_argument.
    ImageBuffer render(const GlyphOutline& outline, uint32_t width, uint32_t height,
                       float scale, float origin_x, float baseline_y) const;
};
}
