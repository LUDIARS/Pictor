#include "pn_gpu_data.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace pictor_kuzuha {
namespace {
constexpr int powers[10][3]{{3,0,0},{0,3,0},{0,0,3},{2,1,0},{1,2,0},
                           {0,2,1},{0,1,2},{1,0,2},{2,0,1},{1,1,1}};
struct DoubleVec {
    double x,y,z;
    DoubleVec(double x,double y,double z):x(x),y(y),z(z) {}
    DoubleVec(Vec v):x(v.x),y(v.y),z(v.z) {}
};
DoubleVec sub(DoubleVec a,DoubleVec b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
DoubleVec add(DoubleVec a,DoubleVec b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
DoubleVec scale(DoubleVec a,double t) { return {a.x*t,a.y*t,a.z*t}; }
double length(DoubleVec a) { return std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z); }
float upward(double value) {
    return std::nextafter(static_cast<float>(value*(1.+1e-12)),std::numeric_limits<float>::infinity());
}
// Direct (i,j) -> control slot lookup. k is implied by i+j+k=3, so a 4x4 table
// covers every valid triple. make_gpu_patch runs this per changed patch per
// frame, so the previous linear scan of `powers` is not affordable here.
// -1 marks an impossible exponent pair (i+j>3).
constexpr int coefficient_slot[4][4]{
    {  2,  6,  5,  1},   // i=0: 003,012,021,030
    {  7,  9,  4, -1},   // i=1: 102,111,120
    {  8,  3, -1, -1},   // i=2: 201,210
    {  0, -1, -1, -1},   // i=3: 300
};
DoubleVec coefficient(const PnPatch& p,int i,int j,int k) {
    if (i<0 || j<0 || k<0 || i+j+k!=3) throw std::logic_error("Invalid PN coefficient powers");
    const int slot=coefficient_slot[i][j];
    if (slot<0) throw std::logic_error("Invalid PN coefficient powers");
    return p.control[slot];
}
/// @implements SPEC-PC-KUZUHA-RAYMARCH
float hessian_bound(const PnPatch& p) {
    double xx=0,xy=0,yy=0;
    // Each second derivative is a degree-one Bernstein patch; its norm
    // is bounded by the largest norm of its three control coefficients.
    for (int corner=0;corner<3;++corner) {
        const int i=corner==0,j=corner==1,k=corner==2;
        const auto base=coefficient(p,i+2,j,k);
        const auto bx=coefficient(p,i+1,j+1,k),by=coefficient(p,i+1,j,k+1);
        xx=std::max(xx,6*length(add(sub(coefficient(p,i,j+2,k),scale(bx,2)),base)));
        yy=std::max(yy,6*length(add(sub(coefficient(p,i,j,k+2),scale(by,2)),base)));
        xy=std::max(xy,6*length(add(sub(sub(coefficient(p,i,j+1,k+1),bx),by),base)));
    }
    return upward(std::max(xx,yy)+xy);
}
}
/// @implements SPEC-PC-KUZUHA-RAYMARCH
std::vector<GpuPnPatch> make_gpu_patches(const PnModel& model) {
    const auto& source=model.source();
    // Patch t owns source triangle t; the corner reads below depend on it.
    if (source.indices.size()!=model.patches().size()*3)
        throw std::runtime_error("PN patch count does not match source triangle count");
    std::vector<GpuPnPatch> result(model.patches().size());
    for (size_t t=0;t<result.size();++t) {
        const auto& p=model.patches()[t];auto& out=result[t];
        out.lower.fill(std::numeric_limits<float>::max());
        out.upper.fill(-std::numeric_limits<float>::max());
        double delta=0;
        for (int n=0;n<10;++n) {
            const Vec b=p.control[n];out.control[n]={b.x,b.y,b.z,0};
            const auto linear=scale(add(add(scale(p.control[0],powers[n][0]),
                scale(p.control[1],powers[n][1])),scale(p.control[2],powers[n][2])),1./3);
            delta=std::max(delta,length(sub(b,linear)));
            for (int axis=0;axis<3;++axis) {
                out.lower[axis]=std::min(out.lower[axis],out.control[n][axis]);
                out.upper[axis]=std::max(out.upper[axis],out.control[n][axis]);
            }
        }
        for (int c=0;c<3;++c) {
            const auto& v=source.vertices[source.indices[t*3+c]];
            out.normals[c]={v.normal[0],v.normal[1],v.normal[2],0};
            if (c<2) { out.uv01[c*2]=v.uv[0];out.uv01[c*2+1]=v.uv[1]; }
            else { out.uv2_bounds[0]=v.uv[0];out.uv2_bounds[1]=v.uv[1]; }
        }
        out.uv2_bounds[2]=hessian_bound(p);
        out.uv2_bounds[3]=upward(delta);
        out.lower[3]=out.upper[3]=0;
    }
    return result;
}
/// @implements SPEC-PC-POLYNOMIAL-MOTION
GpuPnPatch make_gpu_patch(const pictor::demo::PolynomialPatch& source) {
    const PnPatch p{source.control};GpuPnPatch out;
    out.lower.fill(std::numeric_limits<float>::max());
    out.upper.fill(-std::numeric_limits<float>::max());
    double delta=0;
    for (int n=0;n<10;++n) {
        const Vec b=p.control[n];out.control[n]={b.x,b.y,b.z,0};
        const auto linear=scale(add(add(scale(p.control[0],powers[n][0]),
            scale(p.control[1],powers[n][1])),scale(p.control[2],powers[n][2])),1./3);
        delta=std::max(delta,length(sub(b,linear)));
        for (int axis=0;axis<3;++axis) {
            if (!std::isfinite(out.control[n][axis])) throw std::runtime_error("Non-finite motion coefficient");
            out.lower[axis]=std::min(out.lower[axis],out.control[n][axis]);
            out.upper[axis]=std::max(out.upper[axis],out.control[n][axis]);
        }
    }
    for (int c=0;c<3;++c) {
        const auto n=source.normals[c];out.normals[c]={n.x,n.y,n.z,0};
        if (c<2) { out.uv01[c*2]=source.uv[c][0];out.uv01[c*2+1]=source.uv[c][1]; }
        else { out.uv2_bounds[0]=source.uv[c][0];out.uv2_bounds[1]=source.uv[c][1]; }
    }
    out.normals[0][3]=source.color.x;out.normals[1][3]=source.color.y;out.normals[2][3]=source.color.z;
    out.uv2_bounds[2]=hessian_bound(p);out.uv2_bounds[3]=upward(delta);
    out.lower[3]=source.constant_color?1.f:0.f;out.upper[3]=0;
    return out;
}
}
