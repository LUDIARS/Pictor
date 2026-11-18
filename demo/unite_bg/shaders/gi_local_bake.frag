#version 450
#extension GL_GOOGLE_include_directive : require
// =============================================================================
// LocalGI (DynamicReflectionProbe 相当) のベイク:
//   プローブ地点から 6 方向 (+X,-X,+Y,-Y,+Z,-Z) を見て、そこに何があるかの
//   "材質" だけを焼く。照明は焼かない → 実行時に今の太陽・空でライティングし直す。
//   MRT0: Diffuse (rgb = アルベド, a = 物に当たった割合。残りは空)
//   MRT1: Normal  (rgb = 法線*0.5+0.5)
//   MRT2: Emission (rgb = 自己発光)
//   本家は Cubemap + mip でぼかすが、ここは 1 方向 = 1 テクセルを
//   円錐内の複数レイの平均で作る (= ぼかしたのと同じ役割)
// =============================================================================
#include "gi_scene.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_diffuse;
layout(location = 1) out vec4 o_normal;
layout(location = 2) out vec4 o_emission;

void main() {
    int face = int(gl_FragCoord.x);
    const vec3 axes[6] = vec3[](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0),
                                vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));
    vec3 axis = axes[face];

    const int N = 96;
    vec3 alb = vec3(0.0), nrm = vec3(0.0), emi = vec3(0.0);
    float hits = 0.0, wsum = 0.0;
    for (int i = 0; i < N; ++i) {
        vec3 d = fibonacci_dir(i, N);
        float w = max(dot(d, axis), 0.0);
        if (w <= 0.0) continue;
        w *= w;
        wsum += w;
        int id;
        float t = gi_march(LOCAL_PROBE, d, 40.0, false, id);
        if (t < 0.0) continue;
        vec3 p = LOCAL_PROBE + d * t;
        Mat m = gi_material(p, id);
        alb += m.albedo * w;
        nrm += gi_normal(p, false) * w;
        emi += m.emission * w;
        hits += w;
    }
    float hit_ratio = hits / max(wsum, 1e-4);
    o_diffuse  = vec4(alb / max(hits, 1e-4), hit_ratio);
    o_normal   = vec4(normalize(nrm + vec3(1e-5)) * 0.5 + 0.5, 1.0);
    o_emission = vec4(emi / max(wsum, 1e-4), 1.0);
}
