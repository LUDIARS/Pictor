#pragma once

#include "pictor/text/text_types.h"

#include <cstdint>
#include <vector>

namespace pictor {

struct ContourPoint {
    float x = 0.0f, y = 0.0f;
};

/// Closed polylines of a glyph in em units (the last point does not repeat the first).
struct GlyphContours {
    std::vector<ContourPoint> points;
    std::vector<uint32_t>     starts;  // first point of each contour; size = contour count

    [[nodiscard]] uint32_t contour_count() const { return static_cast<uint32_t>(starts.size()); }
    [[nodiscard]] uint32_t contour_end(uint32_t c) const {
        return c + 1 < starts.size() ? starts[c + 1] : static_cast<uint32_t>(points.size());
    }
};

inline constexpr uint32_t kMaxGlyphContourPoints = 65536;

/// Splits every curve so no point of the polyline strays more than
/// `tolerance_em` from the curve. Coordinates are divided by the outline em size.
/// Contours with fewer than 3 points are dropped. Non-finite input, a missing em
/// size or more than `kMaxGlyphContourPoints` points throw std::invalid_argument.
GlyphContours flatten_glyph_contours(const GlyphOutline& outline, float tolerance_em);

/// Nonzero winding number of the contours around a point.
int winding_number(const GlyphContours& contours, ContourPoint p);

} // namespace pictor
