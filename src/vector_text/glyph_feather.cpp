#include "pictor/vector_text/glyph_feather.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace pictor {

namespace {

/// How far from an edge the winding is probed to find its filled side (em).
constexpr float kProbeEm = 1.0e-4f;

struct Band {
    bool  present = false;
    float nx = 0.0f, ny = 0.0f;  // unit, pointing away from the fill
};

Band band_for(const GlyphContours& contours, const ContourPoint& a, const ContourPoint& b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (!(length > 0.0f)) return {};
    const float nx = dy / length, ny = -dx / length;  // right-hand side of a -> b
    const ContourPoint mid{0.5f * (a.x + b.x), 0.5f * (a.y + b.y)};
    const bool right_filled = winding_number(contours, {mid.x + nx * kProbeEm, mid.y + ny * kProbeEm}) != 0;
    const bool left_filled  = winding_number(contours, {mid.x - nx * kProbeEm, mid.y - ny * kProbeEm}) != 0;
    if (right_filled == left_filled) return {};  // inside an overlap, or a cancelled sliver
    return right_filled ? Band{true, -nx, -ny} : Band{true, nx, ny};
}

void push(GlyphMeshData& out, const ContourPoint& p, float ox, float oy, float coverage) {
    out.vertices.push_back({p.x, p.y, ox, oy, coverage});
}

/// Same contours with a point added wherever an edge crosses another edge, so
/// every piece lies wholly on one side of the other contours (an edge can be
/// half buried in an overlapping component).
GlyphContours split_at_crossings(const GlyphContours& in) {
    struct Segment {
        ContourPoint a, b;
    };
    std::vector<Segment> segments;
    segments.reserve(in.points.size());
    for (uint32_t c = 0; c < in.contour_count(); ++c) {
        const uint32_t begin = in.starts[c], end = in.contour_end(c);
        for (uint32_t i = begin; i < end; ++i) {
            segments.push_back({in.points[i], in.points[i + 1 < end ? i + 1 : begin]});
        }
    }
    GlyphContours out;
    std::vector<std::pair<float, ContourPoint>> cuts;
    size_t index = 0;
    for (uint32_t c = 0; c < in.contour_count(); ++c) {
        out.starts.push_back(static_cast<uint32_t>(out.points.size()));
        const uint32_t count = in.contour_end(c) - in.starts[c];
        for (uint32_t i = 0; i < count; ++i, ++index) {
            const Segment& s = segments[index];
            out.points.push_back(s.a);
            cuts.clear();
            const double rx = s.b.x - s.a.x, ry = s.b.y - s.a.y;
            for (size_t j = 0; j < segments.size(); ++j) {
                if (j == index) continue;
                const Segment& o = segments[j];
                const double sx = o.b.x - o.a.x, sy = o.b.y - o.a.y;
                const double denom = rx * sy - ry * sx;
                if (denom == 0.0) continue;  // parallel
                const double qx = o.a.x - s.a.x, qy = o.a.y - s.a.y;
                const double t = (qx * sy - qy * sx) / denom, u = (qx * ry - qy * rx) / denom;
                if (t <= 1e-6 || t >= 1.0 - 1e-6 || u < 0.0 || u > 1.0) continue;
                cuts.emplace_back(static_cast<float>(t), ContourPoint{static_cast<float>(s.a.x + rx * t),
                                                                      static_cast<float>(s.a.y + ry * t)});
            }
            std::sort(cuts.begin(), cuts.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
            for (const auto& cut : cuts) {
                const ContourPoint& last = out.points.back();
                if (cut.second.x != last.x || cut.second.y != last.y) out.points.push_back(cut.second);
            }
        }
    }
    return out;
}

}  // namespace

void add_feather_band(const GlyphContours& original, GlyphMeshData& out) {
    const GlyphContours contours = split_at_crossings(original);
    std::vector<Band> bands;
    for (uint32_t c = 0; c < contours.contour_count(); ++c) {
        const uint32_t begin = contours.starts[c], end = contours.contour_end(c);
        const uint32_t count = end - begin;
        bands.assign(count, Band{});
        for (uint32_t i = 0; i < count; ++i) {
            const ContourPoint& a = contours.points[begin + i];
            const ContourPoint& b = contours.points[begin + (i + 1) % count];
            bands[i] = band_for(original, a, b);
            if (!bands[i].present) continue;
            const Band& n = bands[i];
            const uint32_t base = static_cast<uint32_t>(out.vertices.size());
            push(out, a, 0.0f, 0.0f, 1.0f);
            push(out, b, 0.0f, 0.0f, 1.0f);
            push(out, a, n.nx, n.ny, 0.0f);
            push(out, b, n.nx, n.ny, 0.0f);
            out.indices.insert(out.indices.end(), {base, base + 1, base + 3, base, base + 3, base + 2});
        }
        // Corner wedges: fan between the bands of the two edges meeting at each point.
        for (uint32_t i = 0; i < count; ++i) {
            const Band& in  = bands[(i + count - 1) % count];
            const Band& out_band = bands[i];
            if (!in.present || !out_band.present) continue;
            if (in.nx == out_band.nx && in.ny == out_band.ny) continue;  // straight through
            const ContourPoint& p = contours.points[begin + i];
            const uint32_t base = static_cast<uint32_t>(out.vertices.size());
            push(out, p, 0.0f, 0.0f, 1.0f);
            push(out, p, in.nx, in.ny, 0.0f);
            push(out, p, out_band.nx, out_band.ny, 0.0f);
            out.indices.insert(out.indices.end(), {base, base + 1, base + 2});
        }
    }
}

}  // namespace pictor
