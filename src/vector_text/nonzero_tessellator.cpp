#include "pictor/vector_text/nonzero_tessellator.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace pictor {

namespace {

/// A non-horizontal edge stored bottom (lo) to top (hi). `dir` is +1 when the
/// contour runs upward along it.
struct Edge {
    double x_lo, y_lo, x_hi, y_hi;
    int    dir;

    [[nodiscard]] double x_at(double y) const {
        if (y <= y_lo) return x_lo;  // exact at the ends so shared corners match bit for bit
        if (y >= y_hi) return x_hi;
        return x_lo + (y - y_lo) * (x_hi - x_lo) / (y_hi - y_lo);
    }
};

/// A run of nonzero winding between two edges that is still growing upward.
struct OpenSpan {
    uint32_t left, right;
    double   y_start;
    bool     continued;
};

std::vector<Edge> collect_edges(const GlyphContours& contours) {
    std::vector<Edge> edges;
    edges.reserve(contours.points.size());
    for (uint32_t c = 0; c < contours.contour_count(); ++c) {
        const uint32_t begin = contours.starts[c], end = contours.contour_end(c);
        for (uint32_t i = begin; i < end; ++i) {
            const ContourPoint& a = contours.points[i];
            const ContourPoint& b = contours.points[i + 1 < end ? i + 1 : begin];
            if (a.y == b.y) continue;  // horizontal edges never cross a horizontal scanline
            if (a.y < b.y) {
                edges.push_back({a.x, a.y, b.x, b.y, +1});
            } else {
                edges.push_back({b.x, b.y, a.x, a.y, -1});
            }
        }
    }
    return edges;
}

/// Every y where the order of edges along x can change: ends and crossings.
std::vector<double> slab_boundaries(const std::vector<Edge>& edges) {
    std::vector<double> ys;
    ys.reserve(edges.size() * 2);
    for (const Edge& e : edges) {
        ys.push_back(e.y_lo);
        ys.push_back(e.y_hi);
    }
    for (size_t i = 0; i < edges.size(); ++i) {
        const Edge& a = edges[i];
        for (size_t j = i + 1; j < edges.size(); ++j) {
            const Edge& b = edges[j];
            const double lo = std::max(a.y_lo, b.y_lo), hi = std::min(a.y_hi, b.y_hi);
            if (!(lo < hi)) continue;
            // Signed x gap at both ends of the shared y range; a sign change is a crossing.
            const double g_lo = a.x_at(lo) - b.x_at(lo), g_hi = a.x_at(hi) - b.x_at(hi);
            if ((g_lo < 0.0 && g_hi > 0.0) || (g_lo > 0.0 && g_hi < 0.0)) {
                ys.push_back(lo + (hi - lo) * g_lo / (g_lo - g_hi));
            }
        }
    }
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    return ys;
}

class TrapezoidWriter {
public:
    TrapezoidWriter(const std::vector<Edge>& edges, GlyphMeshData& out) : edges_(edges), out_(out) {}

    void emit(const OpenSpan& span, double y_end) {
        const Edge& l = edges_[span.left];
        const Edge& r = edges_[span.right];
        const float ya = static_cast<float>(span.y_start), yb = static_cast<float>(y_end);
        const float la = static_cast<float>(l.x_at(span.y_start)), ra = static_cast<float>(r.x_at(span.y_start));
        const float lb = static_cast<float>(l.x_at(y_end)), rb = static_cast<float>(r.x_at(y_end));
        const bool bottom = ra > la, top = rb > lb;
        if (!bottom && !top) return;
        const uint32_t base = static_cast<uint32_t>(out_.vertices.size());
        out_.vertices.push_back({la, ya, 0.0f, 0.0f, 1.0f});
        out_.vertices.push_back({ra, ya, 0.0f, 0.0f, 1.0f});
        out_.vertices.push_back({rb, yb, 0.0f, 0.0f, 1.0f});
        out_.vertices.push_back({lb, yb, 0.0f, 0.0f, 1.0f});
        if (bottom) out_.indices.insert(out_.indices.end(), {base, base + 1, base + 2});
        if (top) out_.indices.insert(out_.indices.end(), {base, base + 2, base + 3});
    }

private:
    const std::vector<Edge>& edges_;
    GlyphMeshData&           out_;
};

}  // namespace

void tessellate_nonzero(const GlyphContours& contours, GlyphMeshData& out) {
    std::vector<Edge> edges = collect_edges(contours);
    if (edges.empty()) return;
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.y_lo < b.y_lo; });
    const std::vector<double> ys = slab_boundaries(edges);
    TrapezoidWriter writer(edges, out);

    std::vector<uint32_t> active;
    std::vector<OpenSpan> open, next_open;
    std::vector<std::pair<double, uint32_t>> order;
    size_t next_edge = 0;
    for (size_t s = 0; s + 1 < ys.size(); ++s) {
        const double ya = ys[s], yb = ys[s + 1], mid = 0.5 * (ya + yb);
        // Active edges span the whole slab (slab ends are a superset of edge ends).
        active.erase(std::remove_if(active.begin(), active.end(),
                                    [&](uint32_t e) { return edges[e].y_hi <= ya; }),
                     active.end());
        while (next_edge < edges.size() && edges[next_edge].y_lo <= ya) {
            if (edges[next_edge].y_hi > ya) active.push_back(static_cast<uint32_t>(next_edge));
            ++next_edge;
        }
        order.clear();
        for (uint32_t e : active) order.emplace_back(edges[e].x_at(mid), e);
        std::sort(order.begin(), order.end());

        for (OpenSpan& span : open) span.continued = false;
        next_open.clear();
        int winding = 0;
        uint32_t left = 0;
        for (const auto& [x, e] : order) {
            const int before = winding;
            winding += edges[e].dir;
            if (before == 0 && winding != 0) left = e;
            if (before != 0 && winding == 0) {
                // Same pair of edges as below: the trapezoid simply grows.
                auto same = std::find_if(open.begin(), open.end(), [&](const OpenSpan& o) {
                    return !o.continued && o.left == left && o.right == e;
                });
                if (same != open.end()) {
                    same->continued = true;
                    next_open.push_back({left, e, same->y_start, false});
                } else {
                    next_open.push_back({left, e, ya, false});
                }
            }
        }
        for (const OpenSpan& span : open) {
            if (!span.continued) writer.emit(span, ya);
        }
        open.swap(next_open);
    }
    for (const OpenSpan& span : open) writer.emit(span, ys.back());
}

}  // namespace pictor
