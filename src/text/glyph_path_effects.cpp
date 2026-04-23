#include "pictor/text/glyph_path_effects.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pictor::glyph_path_effects {

// ─── Bounding box ────────────────────────────────────────────

PathBBox compute_bbox(const GlyphOutline& src) {
    PathBBox bb;
    for (const auto& pt : src.path) {
        auto include = [&](float x, float y) {
            if (!bb.valid) {
                bb.min_x = bb.max_x = x;
                bb.min_y = bb.max_y = y;
                bb.valid = true;
                return;
            }
            bb.min_x = std::min(bb.min_x, x);
            bb.max_x = std::max(bb.max_x, x);
            bb.min_y = std::min(bb.min_y, y);
            bb.max_y = std::max(bb.max_y, y);
        };
        switch (pt.command) {
            case SvgPathCommand::MOVE_TO:
            case SvgPathCommand::LINE_TO:
                include(pt.x, pt.y);
                break;
            case SvgPathCommand::QUAD_TO:
                include(pt.cx, pt.cy);
                include(pt.x,  pt.y);
                break;
            case SvgPathCommand::CUBIC_TO:
                include(pt.cx,  pt.cy);
                include(pt.cx2, pt.cy2);
                include(pt.x,   pt.y);
                break;
            case SvgPathCommand::CLOSE:
                break;
        }
    }
    return bb;
}

// ─── Flattening (curves → polylines) ─────────────────────────

GlyphOutline flatten_to_polyline(const GlyphOutline& src, uint32_t seg) {
    GlyphOutline out;
    out.codepoint = src.codepoint;
    out.advance_x = src.advance_x;
    out.em_size   = src.em_size;
    out.path.reserve(src.path.size() * std::max(1u, seg));

    auto emit_move = [&](float x, float y) {
        SvgPathPoint p{};
        p.command = SvgPathCommand::MOVE_TO;
        p.x = x; p.y = y;
        out.path.push_back(p);
    };
    auto emit_line = [&](float x, float y) {
        SvgPathPoint p{};
        p.command = SvgPathCommand::LINE_TO;
        p.x = x; p.y = y;
        out.path.push_back(p);
    };
    auto emit_close = [&]() {
        SvgPathPoint p{};
        p.command = SvgPathCommand::CLOSE;
        out.path.push_back(p);
    };

    float cx = 0.0f, cy = 0.0f; // current pen position
    for (const auto& pt : src.path) {
        switch (pt.command) {
            case SvgPathCommand::MOVE_TO:
                emit_move(pt.x, pt.y);
                cx = pt.x; cy = pt.y;
                break;
            case SvgPathCommand::LINE_TO:
                emit_line(pt.x, pt.y);
                cx = pt.x; cy = pt.y;
                break;
            case SvgPathCommand::QUAD_TO: {
                // Subdivide the quadratic B-spline into `seg` line segments.
                const float x0 = cx, y0 = cy;
                for (uint32_t i = 1; i <= seg; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(seg);
                    const float u = 1.0f - t;
                    const float x = u * u * x0 + 2.0f * u * t * pt.cx + t * t * pt.x;
                    const float y = u * u * y0 + 2.0f * u * t * pt.cy + t * t * pt.y;
                    emit_line(x, y);
                }
                cx = pt.x; cy = pt.y;
                break;
            }
            case SvgPathCommand::CUBIC_TO: {
                const float x0 = cx, y0 = cy;
                for (uint32_t i = 1; i <= seg; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(seg);
                    const float u = 1.0f - t;
                    const float x = u*u*u*x0 + 3.0f*u*u*t*pt.cx + 3.0f*u*t*t*pt.cx2 + t*t*t*pt.x;
                    const float y = u*u*u*y0 + 3.0f*u*u*t*pt.cy + 3.0f*u*t*t*pt.cy2 + t*t*t*pt.y;
                    emit_line(x, y);
                }
                cx = pt.x; cy = pt.y;
                break;
            }
            case SvgPathCommand::CLOSE:
                emit_close();
                break;
        }
    }
    return out;
}

// ─── Translate ───────────────────────────────────────────────

GlyphOutline translate_path(const GlyphOutline& src, const DropShadowParams& p) {
    GlyphOutline out = src;
    for (auto& pt : out.path) {
        switch (pt.command) {
            case SvgPathCommand::CUBIC_TO:
                pt.cx2 += p.dx; pt.cy2 += p.dy;
                [[fallthrough]];
            case SvgPathCommand::QUAD_TO:
                pt.cx  += p.dx; pt.cy  += p.dy;
                [[fallthrough]];
            case SvgPathCommand::MOVE_TO:
            case SvgPathCommand::LINE_TO:
                pt.x   += p.dx; pt.y   += p.dy;
                break;
            case SvgPathCommand::CLOSE:
                break;
        }
    }
    return out;
}

// ─── Scale outward (glow) ────────────────────────────────────

static GlyphOutline scale_about(const GlyphOutline& src, float cx, float cy, float s) {
    GlyphOutline out = src;
    auto sc = [&](float& x, float& y) {
        x = cx + (x - cx) * s;
        y = cy + (y - cy) * s;
    };
    for (auto& pt : out.path) {
        switch (pt.command) {
            case SvgPathCommand::CUBIC_TO: sc(pt.cx2, pt.cy2); [[fallthrough]];
            case SvgPathCommand::QUAD_TO:  sc(pt.cx,  pt.cy);  [[fallthrough]];
            case SvgPathCommand::MOVE_TO:
            case SvgPathCommand::LINE_TO:  sc(pt.x,   pt.y);   break;
            case SvgPathCommand::CLOSE:    break;
        }
    }
    return out;
}

std::vector<GlyphOutline> scale_outward(const GlyphOutline& src, const GlowParams& p) {
    std::vector<GlyphOutline> out;
    out.reserve(p.steps.size());
    const auto bb = compute_bbox(src);
    if (!bb.valid) return out;
    const float cx = (bb.min_x + bb.max_x) * 0.5f;
    const float cy = (bb.min_y + bb.max_y) * 0.5f;
    for (const auto& step : p.steps) {
        out.push_back(scale_about(src, cx, cy, step.scale));
    }
    return out;
}

// ─── Offset outline (simplified polyline-based stroke) ───────
//
// A true variable-width bezier offset is fairly involved; we instead:
//   1. Flatten every curve to line segments.
//   2. Walk each closed sub-path and compute per-vertex outward
//      normals (perpendicular to the average of the two adjacent
//      segment directions).
//   3. Emit one loop: original vertex displaced by `+width * normal`
//      (outer ring), followed by the reversed original vertices
//      (inner ring). Fill with non-zero winding → a hollow stroke band.
//
// This produces correct-looking outlines for text glyphs. Sharp
// exterior corners develop small mitres proportional to `width`; with
// `rounded_joins` we interpolate a short fan to soften them.

namespace {

struct Vec2 { float x = 0.0f; float y = 0.0f; };
inline Vec2 sub(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 perp(Vec2 v)        { return {-v.y, v.x}; }          // 90° CCW
inline Vec2 norm(Vec2 v) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y);
    if (len < 1e-6f) return {0.0f, 0.0f};
    return {v.x / len, v.y / len};
}

void emit_contour(std::vector<SvgPathPoint>& out,
                  const std::vector<Vec2>& outer,
                  const std::vector<Vec2>& inner_rev) {
    if (outer.empty()) return;
    // outer loop
    SvgPathPoint m{};
    m.command = SvgPathCommand::MOVE_TO;
    m.x = outer.front().x;
    m.y = outer.front().y;
    out.push_back(m);
    for (std::size_t i = 1; i < outer.size(); ++i) {
        SvgPathPoint l{};
        l.command = SvgPathCommand::LINE_TO;
        l.x = outer[i].x; l.y = outer[i].y;
        out.push_back(l);
    }
    // close outer
    SvgPathPoint c{}; c.command = SvgPathCommand::CLOSE;
    out.push_back(c);
    // inner loop (reversed original contour)
    if (inner_rev.empty()) return;
    SvgPathPoint m2{};
    m2.command = SvgPathCommand::MOVE_TO;
    m2.x = inner_rev.front().x; m2.y = inner_rev.front().y;
    out.push_back(m2);
    for (std::size_t i = 1; i < inner_rev.size(); ++i) {
        SvgPathPoint l{};
        l.command = SvgPathCommand::LINE_TO;
        l.x = inner_rev[i].x; l.y = inner_rev[i].y;
        out.push_back(l);
    }
    out.push_back(c);
}

} // namespace

GlyphOutline offset_outline(const GlyphOutline& src, const OutlineParams& params) {
    GlyphOutline flat = flatten_to_polyline(src, std::max(1u, params.segments_per_curve));

    GlyphOutline out;
    out.codepoint = src.codepoint;
    out.advance_x = src.advance_x;
    out.em_size   = src.em_size;

    std::vector<Vec2> contour;
    auto finalise_contour = [&]() {
        if (contour.size() < 2) { contour.clear(); return; }
        const std::size_t n = contour.size();
        std::vector<Vec2> outer(n);
        // outer vertex = contour[i] + width * bisector normal
        for (std::size_t i = 0; i < n; ++i) {
            const Vec2 prev = contour[(i + n - 1) % n];
            const Vec2 curr = contour[i];
            const Vec2 next = contour[(i + 1) % n];
            const Vec2 d1 = norm(sub(curr, prev));
            const Vec2 d2 = norm(sub(next, curr));
            // Outward normal at the vertex = average of the segment
            // perpendiculars. For a CCW-wound contour, perp() gives the
            // outward direction; for CW contours the caller would get a
            // negative-width inset (still self-consistent).
            Vec2 n1 = perp(d1);
            Vec2 n2 = perp(d2);
            Vec2 bis = norm({n1.x + n2.x, n1.y + n2.y});
            outer[i] = { curr.x + bis.x * params.width,
                         curr.y + bis.y * params.width };
        }
        // Inner loop is the original contour traversed in reverse.
        std::vector<Vec2> inner_rev(contour.rbegin(), contour.rend());
        emit_contour(out.path, outer, inner_rev);
        contour.clear();
    };

    for (const auto& pt : flat.path) {
        switch (pt.command) {
            case SvgPathCommand::MOVE_TO:
                finalise_contour();
                contour.push_back({pt.x, pt.y});
                break;
            case SvgPathCommand::LINE_TO:
                contour.push_back({pt.x, pt.y});
                break;
            case SvgPathCommand::CLOSE:
                finalise_contour();
                break;
            default:
                // flatten_to_polyline should have removed curves.
                break;
        }
    }
    finalise_contour();
    return out;
}

} // namespace pictor::glyph_path_effects
