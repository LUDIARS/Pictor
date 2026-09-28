// @spec SPEC-PC-VECTOR-TEXT (spec/feature/subsystem/text.md)
#include "pictor/text/glyph_vector_rasterizer.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pictor {
namespace {
struct Point { float x, y; };
struct Edge { Point from, to; int direction; };
struct Crossing { float x; int direction; };
Point midpoint(Point a, Point b) { return {(a.x+b.x)*.5f, (a.y+b.y)*.5f}; }
float distance2(Point a, Point b) { float x=a.x-b.x, y=a.y-b.y; return x*x+y*y; }
constexpr float tolerance2 = .125f*.125f;
constexpr size_t edge_limit = 1048576;
class Edges {
public:
    std::vector<Edge> values;
    void line(Point a, Point b) {
        if (a.y == b.y) return;
        if (values.size() == edge_limit) throw std::invalid_argument("Vector glyph edge budget exceeded");
        values.push_back(a.y < b.y ? Edge{a,b,1} : Edge{b,a,-1});
    }
    void quadratic(Point a, Point c, Point b, unsigned depth = 0) {
        if (distance2(c,midpoint(a,b)) <= tolerance2) { line(a,b); return; }
        if (depth == 20) throw std::invalid_argument("Vector glyph curve cannot be flattened accurately");
        auto ac=midpoint(a,c), cb=midpoint(c,b), m=midpoint(ac,cb);
        quadratic(a,ac,m,depth+1); quadratic(m,cb,b,depth+1);
    }
    void cubic(Point a, Point c, Point d, Point b, unsigned depth = 0) {
        // Compare controls to the equivalent straight cubic, including collinear
        // backtracking curves (a distance-to-line criterion would lose those).
        Point c0{(2*a.x+b.x)/3,(2*a.y+b.y)/3}, d0{(a.x+2*b.x)/3,(a.y+2*b.y)/3};
        if (std::max(distance2(c,c0),distance2(d,d0)) <= tolerance2) { line(a,b); return; }
        if (depth == 20) throw std::invalid_argument("Vector glyph curve cannot be flattened accurately");
        auto ac=midpoint(a,c), cd=midpoint(c,d), db=midpoint(d,b);
        auto left=midpoint(ac,cd), right=midpoint(cd,db), m=midpoint(left,right);
        cubic(a,ac,left,m,depth+1); cubic(m,right,db,b,depth+1);
    }
};
std::vector<Edge> edges_for(const GlyphOutline& outline, float scale, float x, float baseline) {
    Edges edges;
    auto transform=[&](float px, float py) -> Point {
        Point p{px*scale+x, baseline-py*scale};
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || std::abs(p.x)>1e7f || std::abs(p.y)>1e7f)
            throw std::invalid_argument("Vector glyph coordinate out of range");
        return p;
    };
    Point current{}, start{}; bool open=false;
    for (const auto& p : outline.path) {
        if (p.command == SvgPathCommand::MOVE_TO) {
            if (open) edges.line(current,start);
            current=start=transform(p.x,p.y); open=true; continue;
        }
        if (!open) throw std::invalid_argument("Vector glyph contour must start with MOVE_TO");
        if (p.command == SvgPathCommand::CLOSE) { edges.line(current,start); current=start; open=false; continue; }
        const auto end=transform(p.x,p.y);
        switch (p.command) {
        case SvgPathCommand::LINE_TO: edges.line(current,end); break;
        case SvgPathCommand::QUAD_TO: edges.quadratic(current,transform(p.cx,p.cy),end); break;
        case SvgPathCommand::CUBIC_TO: edges.cubic(current,transform(p.cx,p.cy),transform(p.cx2,p.cy2),end); break;
        default: throw std::invalid_argument("Unknown vector glyph path command");
        }
        current=end;
    }
    if (open) edges.line(current,start);
    return std::move(edges.values);
}
void span_coverage(std::vector<float>& coverage, float left, float right) {
    left=std::clamp(left,0.f,float(coverage.size()));
    right=std::clamp(right,0.f,float(coverage.size()));
    if (right <= left) return;
    auto begin=static_cast<size_t>(std::floor(left)), end=static_cast<size_t>(std::ceil(right));
    for (size_t x=begin; x<end; ++x)
        coverage[x] += std::max(0.f,std::min(right,float(x+1))-std::max(left,float(x)));
}
}
ImageBuffer GlyphVectorRasterizer::render(const GlyphOutline& outline, uint32_t width, uint32_t height,
                                          float scale, float origin_x, float baseline_y) const {
    if (!width || !height || width>4096 || height>4096 || outline.path.size()>edge_limit ||
        !std::isfinite(scale) || scale<=0 || !std::isfinite(origin_x) || !std::isfinite(baseline_y))
        throw std::invalid_argument("Invalid vector glyph raster target");
    auto edges=edges_for(outline,scale,origin_x,baseline_y);
    std::sort(edges.begin(),edges.end(),[](const auto& a,const auto& b){return a.from.y<b.from.y;});
    ImageBuffer result; result.allocate(width,height,1);
    std::vector<Crossing> crossings; crossings.reserve(edges.size());
    std::vector<size_t> active; active.reserve(edges.size());
    size_t next_edge=0, remaining_work=32u*1024u*1024u;
    std::vector<float> coverage(width);
    constexpr unsigned samples=8;
    for (uint32_t y=0; y<height; ++y) {
        std::fill(coverage.begin(),coverage.end(),0.f);
        for (unsigned s=0; s<samples; ++s) {
            const float scan=y+(s+.5f)/samples;
            while (next_edge<edges.size() && edges[next_edge].from.y<=scan) {
                if (edges[next_edge].to.y>scan) active.push_back(next_edge);
                ++next_edge;
            }
            active.erase(std::remove_if(active.begin(),active.end(),[&](size_t i){return edges[i].to.y<=scan;}),active.end());
            if (active.size()>remaining_work) throw std::invalid_argument("Vector glyph coverage budget exceeded");
            remaining_work-=active.size();
            crossings.clear();
            for (auto i : active) {
                const auto& edge=edges[i];
                const float t=(scan-edge.from.y)/(edge.to.y-edge.from.y);
                crossings.push_back({edge.from.x+(edge.to.x-edge.from.x)*t,edge.direction});
            }
            std::sort(crossings.begin(),crossings.end(),[](const auto& a,const auto& b){return a.x<b.x;});
            int winding=0; float previous=0;
            for (const auto& crossing : crossings) {
                if (winding) span_coverage(coverage,previous,crossing.x);
                winding+=crossing.direction; previous=crossing.x;
            }
        }
        for (uint32_t x=0; x<width; ++x)
            result.pixels[size_t(y)*width+x]=static_cast<uint8_t>(std::clamp(coverage[x]/samples,0.f,1.f)*255+.5f);
    }
    return result;
}
}
