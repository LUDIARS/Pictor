// @implements SPEC-PC-KUZUHA-STAGE
// Art-directed material presets, not measured skin or a hair-fibre BSDF.
vec3 stageLobe(vec3 base,vec3 n,vec3 v,vec3 l,vec3 energy,float rough,float metal) {
    vec3 h=normalize(v+l);
    float nl=max(dot(n,l),0.),nv=max(dot(n,v),.02),nh=max(dot(n,h),0.);
    float a=rough*rough,a2=a*a;
    float den=nh*nh*(a2-1.)+1.;
    float distribution=a2/(3.14159265*den*den);
    float k=(rough+1.)*(rough+1.)/8.;
    float geometry=nv/(nv*(1.-k)+k)*nl/(nl*(1.-k)+k);
    vec3 f0=mix(vec3(.04),base,metal);
    vec3 fresnel=f0+(1.-f0)*pow(1.-max(dot(v,h),0.),5.);
    vec3 spec=distribution*geometry*fresnel/max(4.*nv*nl,.001);
    return ((1.-fresnel)*base*(1.-metal)/3.14159265+spec)*energy*nl;
}
vec3 stageShade(vec3 base,vec3 n,vec3 point,vec2 uv,float kind) {
    float time=lightDir.w;
    vec3 v=normalize(cameraPos.xyz-point);
    float rough=.58,metal=0.;
    if(kind==1.) rough=.40;
    if(kind==2.) rough=.29;
    if(kind==3.) { rough=.27;metal=.88; }
    // Gem areas remain dielectric; the atlas has deep red gemstone islands.
    if(kind==3. && base.r>base.g*1.6) { rough=.16;metal=0.; }
    // The lower face atlas contains the iris islands; no skin pixel colour heuristic.
    if(kind==1. && uv.y>.75) rough=.22;
    vec3 l=normalize(vec3(1.1*sin(time*.48),.65,.85));
    vec3 result=base*vec3(.16,.18,.25);
    // A tall strip is approximated by three light samples to widen reflections.
    for(int i=-1;i<=1;++i)
        result+=stageLobe(base,n,v,normalize(l+vec3(0,float(i)*.16,0)),vec3(2.6,2.10,1.65)/3.,rough,metal);
    result+=stageLobe(base,n,v,normalize(vec3(-.7,.25,.7)),vec3(.36,.48,.85),rough,metal);
    result+=stageLobe(base,n,v,normalize(vec3(.85,.45,-.7)),vec3(.8,.18,.44),rough,metal);
    return result/(1.+result*.35);
}
