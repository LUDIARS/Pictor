// @implements SPEC-PC-KUZUHA-RAYMARCH
struct PnData {
    vec4 b[10];
    vec4 normals[3];
    vec4 uv01;
    vec4 uv2Bounds; // uv2.xy, Hessian bound, root linear deviation bound
    vec4 lower;
    vec4 upper;
};
layout(std430,set=2,binding=0) readonly buffer PnPatches { PnData patches[]; };
layout(set=0,binding=0) uniform SceneUBO {
    mat4 view;mat4 proj;vec4 lightDir;vec4 lightColor;vec4 cameraPos;
};
layout(push_constant) uniform RayParameters {
    vec4 viewport; // width,height,alpha,display
    vec4 rayRight; // xyz basis, w horizontal half-angle tangent
    vec4 rayUp;    // xyz basis, w vertical half-angle tangent
    vec4 rayForward;
    vec4 quality; // angular tolerance, fixed tolerance (0=auto), FP margin, spare
};
float hitTolerance(float t) { return max(quality.z*8.,quality.y>0. ? quality.y:t*quality.x); }
vec3 pnEvaluate(PnData p,vec2 vw) {
    float v=vw.x,w=vw.y,u=1.-v-w;
    vec3 linear=u*p.b[0].xyz+v*p.b[1].xyz+w*p.b[2].xyz;
    vec3 curved=u*u*u*p.b[0].xyz+v*v*v*p.b[1].xyz+w*w*w*p.b[2].xyz
        +3.*u*u*v*p.b[3].xyz+3.*u*v*v*p.b[4].xyz
        +3.*v*v*w*p.b[5].xyz+3.*v*w*w*p.b[6].xyz
        +3.*u*w*w*p.b[7].xyz+3.*u*u*w*p.b[8].xyz+6.*u*v*w*p.b[9].xyz;
    return mix(linear,curved,viewport.z);
}
void pnTangents(PnData p,vec2 vw,out vec3 dv,out vec3 dw) {
    float v=vw.x,w=vw.y,u=1.-v-w;
    dv=3.*(u*u*(p.b[3].xyz-p.b[0].xyz)+v*v*(p.b[1].xyz-p.b[4].xyz)
        +w*w*(p.b[6].xyz-p.b[7].xyz)+2.*u*v*(p.b[4].xyz-p.b[3].xyz)
        +2.*v*w*(p.b[5].xyz-p.b[9].xyz)+2.*u*w*(p.b[9].xyz-p.b[8].xyz));
    dw=3.*(u*u*(p.b[8].xyz-p.b[0].xyz)+v*v*(p.b[5].xyz-p.b[4].xyz)
        +w*w*(p.b[2].xyz-p.b[7].xyz)+2.*u*v*(p.b[9].xyz-p.b[3].xyz)
        +2.*v*w*(p.b[6].xyz-p.b[9].xyz)+2.*u*w*(p.b[7].xyz-p.b[8].xyz));
    dv=mix(p.b[1].xyz-p.b[0].xyz,dv,viewport.z);
    dw=mix(p.b[2].xyz-p.b[0].xyz,dw,viewport.z);
}
vec3 pnNormal(PnData p,vec2 vw) {
    vec3 dv,dw;pnTangents(p,vw,dv,dw);return cross(dv,dw);
}
float boundsPadding(PnData p) {
    float farthest=length((p.lower.xyz+p.upper.xyz)*.5-cameraPos.xyz)+length(p.upper.xyz-p.lower.xyz);
    return hitTolerance(farthest)*2.+quality.z*4.;
}
