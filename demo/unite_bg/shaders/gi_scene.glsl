// =============================================================================
// gi_scene.glsl — Scene 6 (GI) の地形。ベイク用シェーダーと実行時シェーダーで共有する。
//   ・洞窟 (岩のアーチ)  : 空が遮られる → SkyOcclusion が効く場所
//   ・ショップ (屋根付き): 電球の暖色が回り込む → LocalGI が効く場所
//   ・地面の素材        : 芝 / 砂 / 石畳 で照り返しの色が変わる
//   ・動く箱 (objs[0])  : ベイク後に置かれる物体。ベイクには入らない
// =============================================================================
#ifndef UBG_GI_SCENE_GLSL
#define UBG_GI_SCENE_GLSL

#include "common.glsl"

// プローブグリッド (均一。APV はジオメトリの近くほど細かい点が異なる)
const vec3  PROBE_ORIGIN  = vec3(-16.0, 0.5, -12.0);
const float PROBE_SPACING = 2.0;
const ivec3 PROBE_COUNT   = ivec3(17, 4, 13);

// LocalGI (ショップ) の影響範囲とプローブ位置
const vec3 LOCAL_CENTER = vec3(9.0, 1.6, 0.0);
const vec3 LOCAL_HALF   = vec3(3.6, 1.9, 3.6);
const vec3 LOCAL_PROBE  = vec3(9.0, 1.2, 0.5);

struct Mat { vec3 albedo; vec3 emission; int id; };

// mat id: 0 地面, 1 岩, 2 ショップの壁, 3 屋根(布), 4 電球, 5 木の床, 6 動く箱, 7 プローブ表示
float gi_map(vec3 p, bool dynamic_objects, out int id) {
    float d = p.y; id = 0;

    // 洞窟: 両側の岩 + 天井
    float cave = sd_round_box(p - vec3(-9.0, 1.8, 1.0), vec3(3.2, 1.8, 3.6), 0.4);
    cave = max(cave, -sd_round_box(p - vec3(-9.0, 1.3, 1.0), vec3(1.9, 1.5, 4.5), 0.3));
    cave += (value_noise3(p * 1.7) - 0.5) * 0.25;
    if (cave < d) { d = cave; id = 1; }

    // ショップ: 奥の壁 + 側壁 + 布の屋根 + 柱 + 木の床
    float wall = sd_box(p - vec3(9.0, 1.6, 3.4), vec3(3.4, 1.6, 0.15));
    wall = min(wall, sd_box(p - vec3(5.7, 1.6, 1.0), vec3(0.15, 1.6, 2.4)));
    if (wall < d) { d = wall; id = 2; }
    float roof = sd_box(p - vec3(9.0, 3.3 + 0.15 * sin(p.x * 2.0), 0.4), vec3(3.6, 0.08, 3.2));
    roof = min(roof, sd_capsule(p, vec3(12.4, 0.0, -2.6), vec3(12.4, 3.3, -2.6), 0.12));
    roof = min(roof, sd_capsule(p, vec3(5.8, 0.0, -2.6), vec3(5.8, 3.3, -2.6), 0.12));
    if (roof < d) { d = roof; id = 3; }
    float floor_ = sd_box(p - vec3(9.0, 0.05, 0.4), vec3(3.3, 0.06, 3.0));
    if (floor_ < d) { d = floor_; id = 5; }
    // 電球 (屋根の縁に並ぶ)
    vec3 q = p - vec3(9.0, 3.05, -2.7);
    q.x -= 0.9 * clamp(round(q.x / 0.9), -3.0, 3.0);
    float bulb = sd_sphere(q, 0.12);
    if (bulb < d) { d = bulb; id = 4; }

    if (dynamic_objects && flag(4)) {
        float box = sd_round_box(p - F.objs[0].xyz, vec3(F.objs[0].w), 0.1);
        if (box < d) { d = box; id = 6; }
    }
    return d;
}

Mat gi_material(vec3 p, int id) {
    Mat m; m.emission = vec3(0.0); m.id = id;
    if (id == 0) {
        // 地面: 芝 / 石畳 / 砂
        if (p.z < -4.0)      m.albedo = vec3(0.16, 0.40, 0.12);
        else if (p.x < 4.0)  m.albedo = mix(vec3(0.30, 0.36, 0.55), vec3(0.42, 0.46, 0.62),
                                            step(0.5, fract(floor(p.x) * 0.5 + floor(p.z) * 0.5)));
        else                 m.albedo = vec3(0.78, 0.66, 0.46);
    } else if (id == 1) m.albedo = vec3(0.38, 0.40, 0.55);
    else if (id == 2)   m.albedo = vec3(0.85, 0.80, 0.75);
    else if (id == 3)   m.albedo = vec3(0.20, 0.30, 0.80);
    else if (id == 4) { m.albedo = vec3(1.0); m.emission = vec3(1.0, 0.62, 0.25) * 8.0; }
    else if (id == 5)   m.albedo = vec3(0.55, 0.36, 0.22);
    else                m.albedo = vec3(0.7, 0.7, 0.72);
    return m;
}

float gi_march(vec3 ro, vec3 rd, float tmax, bool dyn, out int id) {
    float t = 0.0; id = -1;
    for (int i = 0; i < 160; ++i) {
        int m;
        float h = gi_map(ro + rd * t, dyn, m);
        if (h < 0.002 + 0.001 * t) { id = m; return t; }
        t += h * 0.9;
        if (t > tmax) break;
    }
    return -1.0;
}

vec3 gi_normal(vec3 p, bool dyn) {
    const vec2 e = vec2(0.01, 0.0);
    int m;
    return normalize(vec3(gi_map(p + e.xyy, dyn, m) - gi_map(p - e.xyy, dyn, m),
                          gi_map(p + e.yxy, dyn, m) - gi_map(p - e.yxy, dyn, m),
                          gi_map(p + e.yyx, dyn, m) - gi_map(p - e.yyx, dyn, m)));
}

// 球面上にほぼ均等に方向を並べる (Fibonacci sphere)
vec3 fibonacci_dir(int i, int n) {
    float k = (float(i) + 0.5) / float(n);
    float phi = 2.399963 * float(i);
    float y = 1.0 - 2.0 * k;
    float r = sqrt(max(1.0 - y * y, 0.0));
    return vec3(cos(phi) * r, y, sin(phi) * r);
}

#endif
