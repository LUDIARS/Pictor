#version 450
#extension GL_GOOGLE_include_directive : require
#include "pn_ray_common.glsl"
layout(location=0) out flat uint patchIndex;
// Only a conservative screen rectangle is rasterized. It supplies no hit,
// surface position, normal, UV or depth; all of those come from the marcher.
void main() {
    patchIndex=gl_InstanceIndex;
    PnData p=patches[patchIndex];
    float padding=boundsPadding(p);
    vec3 lo=p.lower.xyz-vec3(padding),hi=p.upper.xyz+vec3(padding);
    vec2 screenLo=vec2(1e20),screenHi=vec2(-1e20);
    bool crossesNear=false;bool anyInFront=false;
    float nearPlane=proj[3][2]/proj[2][2];
    for (int i=0;i<8;++i) {
        vec3 corner=vec3((i&1)==0?lo.x:hi.x,(i&2)==0?lo.y:hi.y,(i&4)==0?lo.z:hi.z);
        vec4 clip=proj*view*vec4(corner,1.);
        if (clip.w<=nearPlane) crossesNear=true;
        if (clip.w>nearPlane) {
            anyInFront=true;screenLo=min(screenLo,clip.xy/clip.w);screenHi=max(screenHi,clip.xy/clip.w);
        }
    }
    if (crossesNear) { screenLo=vec2(-1.);screenHi=vec2(1.); }
    screenLo=max(screenLo-vec2(3.)/viewport.xy,vec2(-1.));
    screenHi=min(screenHi+vec2(3.)/viewport.xy,vec2(1.));
    const vec2 corners[6]=vec2[](vec2(0,0),vec2(1,0),vec2(0,1),vec2(1,0),vec2(1,1),vec2(0,1));
    if (!anyInFront || any(greaterThan(screenLo,screenHi))) gl_Position=vec4(2,2,0,1);
    else gl_Position=vec4(mix(screenLo,screenHi,corners[gl_VertexIndex]),0,1);
}
