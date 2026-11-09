#include "pictor/vector_text/glyph_contours.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pictor {

namespace {

constexpr uint32_t kMaxSegmentsPerCurve = 256;

class ContourWriter {
public:
    ContourWriter(GlyphContours& out, float inv_em) : out_(out), inv_em_(inv_em) {}

    void move_to(float x, float y) {
        close();
        start_ = static_cast<uint32_t>(out_.points.size());
        open_  = true;
        add(x, y);
    }
    void line_to(float x, float y) {
        if (!open_) move_to(last_x_, last_y_);
        add(x, y);
    }
    void close() {
        if (!open_) return;
        open_ = false;
        // Drop a closing point that repeats the start.
        const uint32_t n = static_cast<uint32_t>(out_.points.size()) - start_;
        if (n >= 2) {
            const ContourPoint& first = out_.points[start_];
            const ContourPoint& last  = out_.points.back();
            if (first.x == last.x && first.y == last.y) out_.points.pop_back();
        }
        if (out_.points.size() - start_ < 3) {
            out_.points.resize(start_);
            return;
        }
        out_.starts.push_back(start_);
    }
    [[nodiscard]] float last_x() const { return last_x_; }
    [[nodiscard]] float last_y() const { return last_y_; }

private:
    void add(float x, float y) {
        last_x_ = x;
        last_y_ = y;
        const ContourPoint p{x * inv_em_, y * inv_em_};
        if (out_.points.size() > start_) {
            const ContourPoint& prev = out_.points.back();
            if (prev.x == p.x && prev.y == p.y) return;  // zero-length segment
        }
        if (out_.points.size() >= kMaxGlyphContourPoints) {
            throw std::invalid_argument("vector_text: glyph outline is too complex");
        }
        out_.points.push_back(p);
    }

    GlyphContours& out_;
    float          inv_em_;
    uint32_t       start_  = 0;
    bool           open_   = false;
    float          last_x_ = 0.0f;
    float          last_y_ = 0.0f;
};

uint32_t segments_for(float deviation, float tolerance) {
    if (!(deviation > tolerance)) return 1;
    const float n = std::ceil(std::sqrt(deviation / tolerance));
    return static_cast<uint32_t>(std::min(n, static_cast<float>(kMaxSegmentsPerCurve)));
}

bool finite(const SvgPathPoint& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.cx) && std::isfinite(p.cy) &&
           std::isfinite(p.cx2) && std::isfinite(p.cy2);
}

}  // namespace

GlyphContours flatten_glyph_contours(const GlyphOutline& outline, float tolerance_em) {
    if (!(outline.em_size > 0.0f) || !std::isfinite(outline.em_size)) {
        throw std::invalid_argument("vector_text: outline has no em size");
    }
    if (!(tolerance_em > 0.0f)) throw std::invalid_argument("vector_text: tolerance must be positive");
    // Work in font units; the tolerance follows.
    const float tolerance = tolerance_em * outline.em_size;
    GlyphContours out;
    ContourWriter writer(out, 1.0f / outline.em_size);
    for (const SvgPathPoint& p : outline.path) {
        if (!finite(p)) throw std::invalid_argument("vector_text: non-finite outline point");
        switch (p.command) {
            case SvgPathCommand::MOVE_TO:
                writer.move_to(p.x, p.y);
                break;
            case SvgPathCommand::LINE_TO:
                writer.line_to(p.x, p.y);
                break;
            case SvgPathCommand::QUAD_TO: {
                const float x0 = writer.last_x(), y0 = writer.last_y();
                // Chord error of n uniform pieces is |p0 - 2c + p1| / (4 n^2).
                const float dx = x0 - 2.0f * p.cx + p.x, dy = y0 - 2.0f * p.cy + p.y;
                const uint32_t n = segments_for(std::sqrt(dx * dx + dy * dy) / 4.0f, tolerance);
                for (uint32_t i = 1; i <= n; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(n), u = 1.0f - t;
                    writer.line_to(u * u * x0 + 2.0f * u * t * p.cx + t * t * p.x,
                                   u * u * y0 + 2.0f * u * t * p.cy + t * t * p.y);
                }
                break;
            }
            case SvgPathCommand::CUBIC_TO: {
                const float x0 = writer.last_x(), y0 = writer.last_y();
                // |B''| <= 6 max(|p0 - 2c1 + c2|, |c1 - 2c2 + p1|); chord error <= |B''| / (8 n^2).
                const float ax = x0 - 2.0f * p.cx + p.cx2, ay = y0 - 2.0f * p.cy + p.cy2;
                const float bx = p.cx - 2.0f * p.cx2 + p.x, by = p.cy - 2.0f * p.cy2 + p.y;
                const float m = std::max(std::sqrt(ax * ax + ay * ay), std::sqrt(bx * bx + by * by));
                const uint32_t n = segments_for(m * 0.75f, tolerance);
                for (uint32_t i = 1; i <= n; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(n), u = 1.0f - t;
                    const float w0 = u * u * u, w1 = 3.0f * u * u * t, w2 = 3.0f * u * t * t, w3 = t * t * t;
                    writer.line_to(w0 * x0 + w1 * p.cx + w2 * p.cx2 + w3 * p.x,
                                   w0 * y0 + w1 * p.cy + w2 * p.cy2 + w3 * p.y);
                }
                break;
            }
            case SvgPathCommand::CLOSE:
                writer.close();
                break;
            default:
                throw std::invalid_argument("vector_text: unknown path command");
        }
    }
    writer.close();
    return out;
}

int winding_number(const GlyphContours& contours, ContourPoint p) {
    int winding = 0;
    for (uint32_t c = 0; c < contours.contour_count(); ++c) {
        const uint32_t begin = contours.starts[c], end = contours.contour_end(c);
        for (uint32_t i = begin; i < end; ++i) {
            const ContourPoint& a = contours.points[i];
            const ContourPoint& b = contours.points[i + 1 < end ? i + 1 : begin];
            if (a.y <= p.y) {
                if (b.y > p.y && (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y) > 0.0f) ++winding;
            } else if (b.y <= p.y && (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y) < 0.0f) {
                --winding;
            }
        }
    }
    return winding;
}

}  // namespace pictor
