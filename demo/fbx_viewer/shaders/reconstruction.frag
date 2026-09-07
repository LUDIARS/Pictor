#version 450
layout(location=0) in vec3 fragWorldPos;
layout(location=1) in vec3 fragNormal;
layout(location=2) in vec2 fragUV;
layout(location=3) in flat uint fragInstanceID;
layout(location=4) in float fragBindGroove;
layout(set=0,binding=0) uniform SceneUBO { mat4 view;mat4 proj;vec4 lightDir;vec4 lightColor;vec4 cameraPos; };
struct InstanceData { mat4 model;vec4 baseColor;uvec4 skinInfo; };
layout(std430,set=0,binding=1) readonly buffer InstanceBuffer { InstanceData instances[]; };
layout(set=1,binding=0) uniform sampler2D uDiffuse;
layout(location=0) out vec4 outColor;
void main() {
    uint mode=instances[fragInstanceID].skinInfo.z;
    vec4 tex=texture(uDiffuse,fragUV);
    // Preserve authored cutout masks for eyelashes and hair cards.
    if (tex.a<.35) discard;
    vec3 n=normalize(fragNormal);
    if (mode==2) {
        vec3 geometric=normalize(cross(dFdx(fragWorldPos),dFdy(fragWorldPos)));
        n=dot(geometric,n)<0. ? -geometric:geometric;
    }
    vec3 base=mode==0 ? tex.rgb:vec3(.68,.72,.77);
    float key=max(dot(n,normalize(vec3(-.45,.7,.65))),0.);
    float fill=max(dot(n,normalize(vec3(.7,.2,-.5))),0.);
    outColor=vec4(base*(.27+.65*key+.18*fill),1.);
}
