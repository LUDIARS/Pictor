#include "pn_patch.h"
#include <cmath>

namespace pictor_kuzuha {
namespace {
Vec add(Vec a, Vec b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec sub(Vec a, Vec b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec scale(Vec a, float s) { return {a.x*s,a.y*s,a.z*s}; }
float dot(Vec a, Vec b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec edge_control(Vec a, Vec b, Vec n, bool straight) {
    // Vlachos et al., Curved PN Triangles (I3D 2001), section 3.1.
    return scale(sub(add(scale(a,2),b), scale(n,straight ? 0.f : dot(sub(b,a),n))),1.f/3);
}
}
Vec unit(Vec v) {
    const float length = std::sqrt(dot(v,v));
    return length > 1e-12f ? scale(v,1/length) : Vec{0,0,0};
}
PnPatch make_patch(const std::array<Vec,3>& p, const std::array<Vec,6>& n,
                   const std::array<bool,3>& straight) {
    PnPatch patch;
    auto& b = patch.control;
    b[0]=p[0]; b[1]=p[1]; b[2]=p[2];
    b[3]=edge_control(p[0],p[1],n[0],straight[0]);
    b[4]=edge_control(p[1],p[0],n[1],straight[0]);
    b[5]=edge_control(p[1],p[2],n[2],straight[1]);
    b[6]=edge_control(p[2],p[1],n[3],straight[1]);
    b[7]=edge_control(p[2],p[0],n[4],straight[2]);
    b[8]=edge_control(p[0],p[2],n[5],straight[2]);
    Vec e{};
    for (int i=3;i<9;++i) e=add(e,b[i]);
    e=scale(e,1.f/6);
    const Vec center=scale(add(add(p[0],p[1]),p[2]),1.f/3);
    b[9]=add(e,scale(sub(e,center),.5f));
    return patch;
}
Vec evaluate(const PnPatch& p, float u, float v, float w) {
    const std::array<float,10> weights{u*u*u,v*v*v,w*w*w,3*u*u*v,
        3*u*v*v,3*v*v*w,3*v*w*w,3*u*w*w,3*u*u*w,6*u*v*w};
    Vec result{};
    for (int i=0;i<10;++i) result=add(result,scale(p.control[i],weights[i]));
    return result;
}
}
