#version 450
#extension GL_GOOGLE_include_directive : require
// =============================================================================
// Scene 3 / Pass A: 街のシーンを描き、RGB = 色, A = カメラからの距離 [m] を
// オフスクリーンに出力する。Pass B (shaft_composite) はこの A を
// 「深度テクスチャ」として使い、遮蔽をレイマーチする。
//   p1: x = 霞 (空気遠近) の濃さ
//   objs[0..9]: 建物 (xyz = 中心, w = 高さ)
// =============================================================================
#include "atmosphere.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

const float FAR = 1e4;

float map(vec3 p, out int mat) {
    float d = p.y; mat = 0;                               // 地面
    for (int i = 0; i < 10; ++i) {
        vec4 b = F.objs[i];
        float h = b.w;
        float db = sd_box(p - vec3(b.x, h * 0.5, b.z), vec3(4.0 + 2.0 * fract(float(i) * 0.37), h * 0.5, 4.0));
        if (db < d) { d = db; mat = 1; }
    }
    // 時計塔
    float tower = sd_box(p - vec3(-6.0, 18.0, 40.0), vec3(2.0, 18.0, 2.0));
    tower = min(tower, sd_capsule(p, vec3(-6.0, 36.0, 40.0), vec3(-6.0, 42.0, 40.0), 1.2));
    if (tower < d) { d = tower; mat = 2; }
    // 木 (球 + 幹)
    for (int i = 0; i < 6; ++i) {
        vec3 c = vec3(-20.0 + float(i) * 8.0, 0.0, 18.0 + 6.0 * sin(float(i) * 1.7));
        float t = min(sd_sphere(p - c - vec3(0.0, 6.0, 0.0), 3.2),
                      sd_capsule(p, c, c + vec3(0.0, 4.0, 0.0), 0.35));
        if (t < d) { d = t; mat = 3; }
    }
    return d;
}
float map_d(vec3 p) { int m; return map(p, m); }

vec3 calc_normal(vec3 p) {
    const vec2 e = vec2(0.01, 0.0);
    return normalize(vec3(map_d(p + e.xyy) - map_d(p - e.xyy),
                          map_d(p + e.yxy) - map_d(p - e.yxy),
                          map_d(p + e.yyx) - map_d(p - e.yyx)));
}
float soft_shadow(vec3 ro, vec3 rd) {
    float res = 1.0, t = 0.05;
    for (int i = 0; i < 48 && t < 120.0; ++i) {
        float h = map_d(ro + rd * t);
        if (h < 0.001) return 0.0;
        res = min(res, 10.0 * h / t);
        t += clamp(h, 0.05, 4.0);
    }
    return clamp(res, 0.0, 1.0);
}

void main() {
    vec3 ro = F.cam_pos.xyz;
    vec3 rd = camera_ray(v_uv);

    float t = 0.0; int mat = -1;
    for (int i = 0; i < 160; ++i) {
        int m;
        float h = map(ro + rd * t, m);
        if (h < 0.002 * t + 0.001) { mat = m; break; }
        t += h;
        if (t > 600.0) break;
    }

    vec3 sky = sky_color(rd, 12, 6);
    if (mat < 0) {
        o_color = vec4(sky + sun_disk(rd), FAR);
        return;
    }
    vec3 p = ro + rd * t;
    vec3 n = calc_normal(p);
    vec3 albedo = mat == 0 ? vec3(0.32, 0.30, 0.28)
                : mat == 1 ? vec3(0.62, 0.55, 0.50)
                : mat == 2 ? vec3(0.70, 0.62, 0.55)
                           : vec3(0.18, 0.32, 0.12);
    vec3 L = F.sun_dir.xyz;
    float sh = soft_shadow(p + n * 0.02, L);
    vec3 col = albedo * (sun_light_at_ground() * max(dot(n, L), 0.0) * sh
                       + sky_ambient_up() * (0.5 + 0.5 * n.y) * 0.35);

    // 霞 (空気遠近): 遠いほど元の色が薄れて空気の色 (= その方向の空の色) に近づく
    float fog = 1.0 - exp(-t * F.p[1].x);
    col = mix(col, sky, fog);

    o_color = vec4(col, t);
}
