#version 450
#extension GL_GOOGLE_include_directive : require
#include "pn_ray_common.glsl"
#include "pn_ray_distance.glsl"
layout(location=0) in flat uint patchIndex;
layout(location=0) out vec4 outColor;
layout(set=1,binding=0) uniform sampler2D uDiffuse;
layout(std430,set=2,binding=1) buffer RayCounters { uint unresolved;uint spare0;uint spare1;uint spare2; };
#include "pn_stage.glsl"

void main() {
    PnData surface=patches[patchIndex];
    vec2 ndc=2.*gl_FragCoord.xy/viewport.xy-1.;
    vec3 direction=normalize(rayForward.xyz+rayRight.xyz*ndc.x*rayRight.w-rayUp.xyz*ndc.y*rayUp.w);
    float padding=boundsPadding(surface),t,end;
    if (!rayBounds(cameraPos.xyz,direction,surface.lower.xyz-vec3(padding),surface.upper.xyz+vec3(padding),t,end)) discard;
    // Clip the search interval before finding a hit (discarding a near-clipped
    // first hit afterwards would incorrectly lose a later visible hit).
    // Convert the depth-range planes into ray parameters via the axial
    // component. A ray at or behind the view plane has no positive axial
    // distance, so it can never satisfy the depth range at all.
    float axial=dot(direction,rayForward.xyz);
    if (axial<=1e-6) discard;
    float nearPlane=proj[3][2]/proj[2][2];
    float farPlane=proj[3][2]/(proj[2][2]+1.);
    t=max(t,nearPlane/axial);end=min(end,farPlane/axial);
    if (t>end) discard;
    vec2 uv=vec2(0);bool hit=false,failed=false;
    int steps=0;
    for (;steps<512 && t<=end;++steps) {
        float lower,upper;vec3 point=cameraPos.xyz+t*direction;
        float epsilon=hitTolerance(t);
        if (!boundedDistance(surface,point,epsilon,lower,upper,uv)) { failed=true;break; }
        if (upper<=epsilon) { hit=true;break; }
        float next=t+lower;
        if (!(next>t)) { failed=true;break; }
        t=next;
    }
    if (!hit && !failed && t>end) discard;
    if (!hit) {
        // Budget exhaustion is visible and counted, never silently called MISS.
        // Clamp t back into the marched interval: a failed march can leave t
        // past `end`, which the shared depth test below would then reject,
        // discarding the fragment after it was already counted. The counter and
        // the magenta pixels must agree, or the HUD reports failures that are
        // nowhere on screen.
        t=clamp(t,nearPlane/axial,end);
        atomicAdd(unresolved,1);
        outColor=vec4(1.,0.,.65,1.);
    } else {
        float u=1.-uv.x-uv.y;
        vec2 texUV=u*surface.uv01.xy+uv.x*surface.uv01.zw+uv.y*surface.uv2Bounds.xy;
        vec4 tex=surface.lower.w>0.?vec4(surface.normals[0].w,surface.normals[1].w,surface.normals[2].w,1.):textureLod(uDiffuse,texUV,0.);
        if (tex.a<.35) discard;
        vec3 imported=u*surface.normals[0].xyz+uv.x*surface.normals[1].xyz+uv.y*surface.normals[2].xyz;
        vec3 n=imported;
        if (viewport.w==2.) {
            n=pnNormal(surface,uv);if (dot(n,imported)<0.) n=-n;
        }
        n=dot(n,n)>1e-20?normalize(n):vec3(0,1,0);
        vec3 base=viewport.w==0.?tex.rgb:vec3(.68,.72,.77);
        float key=max(dot(n,normalize(vec3(-.45,.7,.65))),0.);
        float fill=max(dot(n,normalize(vec3(.7,.2,-.5))),0.);
        outColor=vec4(base*(.27+.65*key+.18*fill),1.);
        if (lightColor.w<0. && viewport.w==0.)
            outColor=vec4(stageShade(base,n,cameraPos.xyz+t*direction,texUV,surface.upper.w),1.);
        if (viewport.w==3.) {
            float heat=clamp(float(steps)/80.,0.,1.);
            outColor=vec4(mix(vec3(.08,.3,.85),vec3(1.,.65,.05),heat),1.);
        }
    }
    vec4 clip=proj*view*vec4(cameraPos.xyz+t*direction,1.);
    if (clip.w<=0. || clip.z<0. || clip.z>clip.w) discard;
    gl_FragDepth=clip.z/clip.w;
}
