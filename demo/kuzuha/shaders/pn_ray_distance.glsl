// @implements SPEC-PC-KUZUHA-RAYMARCH
// Closest point on a triangle, including degenerate edges/points.
vec2 closestTriangle(vec3 point,vec3 a,vec3 b,vec3 c,out float distanceToTriangle) {
    vec3 e=b-a,f=c-a,q=point-a;
    vec3 normal=cross(e,f);
    float ee=dot(e,e),ff=dot(f,f),det=dot(normal,normal);
    vec2 uv=vec2(0);float best=dot(q,q);
    if (det>0.) {
        // Cross products avoid cancellation in ee*ff-ef*ef for skinny faces.
        vec2 v=vec2(dot(cross(q,f),normal),dot(cross(e,q),normal))/det;
        if (v.x>=0. && v.y>=0. && v.x+v.y<=1.) {
            distanceToTriangle=length(q-v.x*e-v.y*f);return v;
        }
    }
    float s=ee>0. ? clamp(dot(q,e)/ee,0.,1.):0.;
    vec3 d=q-s*e;float d2=dot(d,d);
    if (d2<best) { best=d2;uv=vec2(s,0); }
    s=ff>0. ? clamp(dot(q,f)/ff,0.,1.):0.;
    d=q-s*f;d2=dot(d,d);
    if (d2<best) { best=d2;uv=vec2(0,s); }
    vec3 edge=c-b;float e2=dot(edge,edge);
    s=e2>0. ? clamp(dot(point-b,edge)/e2,0.,1.):0.;
    d=point-b-s*edge;d2=dot(d,d);
    if (d2<best) { best=d2;uv=vec2(1.-s,s); }
    distanceToTriangle=sqrt(max(best,0.));return uv;
}
// Full-domain branch-and-bound distance query. A frontier triangle's linear
// surrogate is within delta=alpha*H*scale^2/4 of the actual polynomial patch.
// Lower bounds from ALL unpruned domains are retained, never only the nearest
// surrogate. Upper bounds are evaluated actual PN points and used only for hits
// and pruning; they are never used to advance the ray.
// Capacity and the overflow guard below must stay in lockstep; a guard that
// drifts from the array size is an out-of-bounds write in a shader, which most
// drivers do not trap.
#define PN_STACK_CAPACITY 28
bool boundedDistance(PnData p,vec3 point,float epsilon,out float lower,out float upper,out vec2 bestUV) {
    vec3 stack[PN_STACK_CAPACITY];int pending=1;stack[0]=vec3(0,0,1);
    lower=1e30;upper=1e30;bestUV=vec2(0);
    for (int visit=0;visit<768 && pending>0;++visit) {
        vec3 node=stack[--pending];vec2 origin=node.xy;float s=node.z;
        vec3 a=pnEvaluate(p,origin),b=pnEvaluate(p,origin+vec2(s,0)),c=pnEvaluate(p,origin+vec2(0,s));
        float triangleDistance;
        vec2 uv=origin+s*closestTriangle(point,a,b,c,triangleDistance);
        float delta=viewport.z*p.uv2Bounds.z*s*s*.25;
        if (abs(s)==1.) delta=min(delta,viewport.z*p.uv2Bounds.w);
        float bound=max(triangleDistance-delta-quality.z,0.);
        if (bound>=upper) continue;
        float candidate=length(point-pnEvaluate(p,uv));
        if (candidate<upper) { upper=candidate;bestUV=uv; }
        if (upper<=epsilon) { lower=0.;return true; }
        if (delta<=max(epsilon*.125,bound*.25) || abs(s)<=1./512.) {
            lower=min(lower,bound);continue;
        }
        // Out of stack: the node cannot be refined, but its own bound is
        // already conservative. Retain it as a frontier leaf rather than
        // failing the pixel — a coarser step is correct, just slower.
        if (pending+4>PN_STACK_CAPACITY) { lower=min(lower,bound);continue; }
        float h=s*.5;
        stack[pending++]=vec3(origin+vec2(h,h),-h);
        stack[pending++]=vec3(origin+vec2(0,h),h);
        stack[pending++]=vec3(origin+vec2(h,0),h);
        stack[pending++]=vec3(origin,h);
    }
    // Only an exhausted visit budget is a failure. A completed traversal that
    // converges to lower==0 without an epsilon hit is a valid (if unhelpful)
    // bound; the caller's no-progress check turns it into UNRESOLVED, so
    // reporting it as failure here would also strand legitimate near-surface
    // points at grazing incidence.
    if (pending>0) return false;
    lower=min(lower,upper);
    return true;
}
bool rayBounds(vec3 origin,vec3 direction,vec3 lo,vec3 hi,out float nearT,out float farT) {
    nearT=0.;farT=1e30;
    for (int axis=0;axis<3;++axis) {
        if (abs(direction[axis])<1e-10) {
            if (origin[axis]<lo[axis] || origin[axis]>hi[axis]) return false;
        } else {
            float a=(lo[axis]-origin[axis])/direction[axis];float b=(hi[axis]-origin[axis])/direction[axis];
            nearT=max(nearT,min(a,b));farT=min(farT,max(a,b));
        }
    }
    return nearT<=farT;
}
