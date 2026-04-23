#pragma once

/// pictor::glyph_path_effects — transform `GlyphOutline` path data for
/// outline / drop-shadow / glow effects, *without* rasterising first.
///
/// Operates entirely on `std::vector<SvgPathPoint>`. The transforms
/// produce new `GlyphOutline`s that can be fed back to any path
/// renderer (Pictor's internal scanline rasterizer, Rive runtime,
/// Lottie, or exported via `TextSvgRenderer`). This gives **resolution-
/// independent** outline / shadow / glow — useful for very large text
/// or when you want the effect to stay crisp at any scale.
///
/// For a simpler bitmap-based workflow see `text_effects.h`.

#include <cstdint>
#include <vector>

#include "pictor/text/text_types.h"

namespace pictor::glyph_path_effects {

// ─── Outline (offset the contour outward) ─────────────────────
struct OutlineParams {
    /// Offset distance in font units (em coordinates). The returned
    /// outline is a new filled shape whose outer contour is the
    /// original path pushed outward by `width`. Negative values inset.
    float    width        = 20.0f;
    /// Subdivision count for each bezier segment. Higher values produce
    /// smoother offsets but larger paths. 8 is a good default.
    uint32_t segments_per_curve = 8;
    /// Miter vs. round joins. Rounded joins are produced by adding
    /// short arc approximations at vertices with sharp turns.
    bool     rounded_joins = true;
};

/// Return a new `GlyphOutline` whose path describes the outline shape
/// (the band between the original contour and its outward offset).
/// The result is two sub-paths per original sub-path — an outer loop
/// (offset outward) and an inner loop (the original, reversed). Filled
/// with an even-odd or non-zero rule, this produces a hollow stroke.
GlyphOutline offset_outline(const GlyphOutline& src, const OutlineParams& params);

// ─── Drop shadow (translate the whole path) ───────────────────
struct DropShadowParams {
    float dx = 4.0f;    ///< In font units; positive = right
    float dy = 4.0f;    ///< Positive = down (y grows downward per SVG)
};

/// Translate every point of `src.path` by (dx, dy). Trivial transform
/// — but having a named helper makes the intent explicit in calling
/// code and keeps the effect pipeline uniform with outline / glow.
GlyphOutline translate_path(const GlyphOutline& src, const DropShadowParams& params);

// ─── Glow (scale outward from bbox centre) ────────────────────
struct GlowStep {
    /// Scale factor relative to original (>1 expands outward).
    float scale = 1.1f;
};

struct GlowParams {
    /// Typical call stacks 4-6 steps with increasing scale so the
    /// outermost ring fades out. Each layer is filled at a progressively
    /// lower alpha by the caller (see documentation).
    std::vector<GlowStep> steps = { {1.1f}, {1.2f}, {1.35f} };
};

/// Return `steps.size()` scaled copies of `src`. Each copy is scaled
/// about the bounding-box centre of the original path. Caller fills
/// the returned paths behind the glyph, typically with descending alpha
/// and a glow colour — this approximates a path-native glow without
/// bitmap blurring.
std::vector<GlyphOutline> scale_outward(const GlyphOutline& src, const GlowParams& params);

// ─── Utility: bounding box ────────────────────────────────────
struct PathBBox {
    float min_x = 0.0f, min_y = 0.0f;
    float max_x = 0.0f, max_y = 0.0f;
    bool  valid = false;
};
PathBBox compute_bbox(const GlyphOutline& src);

// ─── Utility: subdivide all curves into line segments ─────────

/// Replace every QUAD_TO / CUBIC_TO with a chain of LINE_TO
/// approximations. Useful when the downstream consumer only supports
/// polyline input. `segments_per_curve` controls the subdivision rate.
GlyphOutline flatten_to_polyline(const GlyphOutline& src, uint32_t segments_per_curve);

} // namespace pictor::glyph_path_effects
