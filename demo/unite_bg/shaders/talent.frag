#version 450
#extension GL_GOOGLE_include_directive : require
// =============================================================================
// Scene 7: タレント (キャラクター) のライティング
//   p0: x=Light Mode (0 太陽そのまま / 1 太陽からズラす / 2 固定)  y=Offset Yaw(deg)
//       z=Fixed Yaw(deg)  w=Rim Power
//   p1: x=Rim Intensity  y=Rim Shadow Atten (影の中のリムの残り具合)
//       z=GI Intensity   w=GI Saturation
//   p2: x=GI Correction (肌の色を戻す量)  y=GI Boost
//   objs[0].x = キャラの向き (rad)
//   toggle Z: リム  X: GI 補正  C: 屋根 (キャラを影に入れる)  V: 回転台
// =============================================================================
#include "atmosphere.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

// mat: 0 地面, 1 柱, 2 肌, 3 髪, 4 服, 5 脚, 6 目, 7 屋根
vec3 to_char(vec3 p) {
    float a = F.objs[0].x;
    return vec3(cos(a) * p.x - sin(a) * p.z, p.y, sin(a) * p.x + cos(a) * p.z);
}

float map(vec3 p, out int mat) {
    float d = p.y; mat = 0;
    // 背景の柱
    vec3 q = p - vec3(0.0, 0.0, 9.0);
    q.x -= 4.0 * clamp(round(q.x / 4.0), -3.0, 3.0);
    float pil = sd_cyl_y(q - vec3(0.0, 2.5, 0.0), 0.4, 2.5);
    if (pil < d) { d = pil; mat = 1; }
    if (flag(2)) {
        float roof = sd_box(p - vec3(0.0, 3.2, 0.5), vec3(2.2, 0.06, 2.0));
        if (roof < d) { d = roof; mat = 7; }
    }

    vec3 c = to_char(p);
    if (length(c - vec3(0.0, 1.0, 0.0)) > 1.6) return min(d, length(c - vec3(0.0, 1.0, 0.0)) - 1.5);

    // 頭と顔
    float head = sd_sphere(c - vec3(0.0, 1.42, 0.0), 0.40);
    if (head < d) { d = head; mat = 2; }
    // 髪: 少し大きい球から顔の前面をくり抜く + 後ろ髪
    float hair = sd_sphere(c - vec3(0.0, 1.50, 0.05), 0.45);
    hair = max(hair, -sd_box(c - vec3(0.0, 1.33, -0.45), vec3(0.34, 0.24, 0.35)));
    hair = smin(hair, sd_round_box(c - vec3(0.0, 1.05, 0.22), vec3(0.38, 0.45, 0.10), 0.08), 0.08);
    if (hair < d) { d = hair; mat = 3; }
    // 目
    vec3 e = vec3(abs(c.x) - 0.14, c.y - 1.38, c.z + 0.37);
    float eye = sd_sphere(e * vec3(1.0, 0.75, 1.0), 0.065);
    if (eye < d) { d = eye; mat = 6; }
    // 体・スカート
    float body = sd_capsule(c, vec3(0.0, 0.62, 0.0), vec3(0.0, 0.95, 0.0), 0.20);
    float skirt = sd_cyl_y(c - vec3(0.0, 0.52, 0.0), 0.30 - (c.y - 0.52) * 0.4, 0.14) - 0.03;
    body = smin(body, skirt, 0.05);
    if (body < d) { d = body; mat = 4; }
    // 腕
    vec3 ac = vec3(abs(c.x), c.y, c.z);
    float arm = sd_capsule(ac, vec3(0.20, 0.92, 0.0), vec3(0.34, 0.62, -0.02), 0.065);
    if (arm < d) { d = arm; mat = 2; }
    // 脚
    float leg = sd_capsule(ac, vec3(0.10, 0.40, 0.0), vec3(0.10, 0.05, 0.0), 0.075);
    if (leg < d) { d = leg; mat = 5; }
    return d;
}
float map_d(vec3 p) { int m; return map(p, m); }
vec3 calc_normal(vec3 p) {
    const vec2 e = vec2(0.002, 0.0);
    return normalize(vec3(map_d(p + e.xyy) - map_d(p - e.xyy),
                          map_d(p + e.yxy) - map_d(p - e.yxy),
                          map_d(p + e.yyx) - map_d(p - e.yyx)));
}
float shadow(vec3 ro, vec3 rd) {
    float res = 1.0, t = 0.02;
    for (int i = 0; i < 64 && t < 30.0; ++i) {
        float h = map_d(ro + rd * t);
        if (h < 0.0005) return 0.0;
        res = min(res, 12.0 * h / t);
        t += clamp(h, 0.01, 1.0);
    }
    return clamp(res, 0.0, 1.0);
}

vec3 rot_y(vec3 v, float a) { return vec3(cos(a) * v.x + sin(a) * v.z, v.y, -sin(a) * v.x + cos(a) * v.z); }

vec3 saturate_color(vec3 c, float s) { return mix(vec3(luminance(c)), c, s); }

void main() {
    vec3 ro = F.cam_pos.xyz;
    vec3 rd = camera_ray(v_uv);
    vec3 sun = sun_light_at_ground();
    vec3 L = F.sun_dir.xyz;

    float t = 0.0; int mat = -1;
    for (int i = 0; i < 200; ++i) {
        int m;
        float h = map(ro + rd * t, m);
        if (h < 0.0005 + 0.0005 * t) { mat = m; break; }
        t += h;
        if (t > 120.0) break;
    }
    vec3 sky_up = sky_ambient_up(), sky_hor = sky_ambient_horizon();
    if (mat < 0) { o_color = finalize(sky_color(rd, 12, 6) + sun_disk(rd)); return; }

    vec3 p = ro + rd * t;
    vec3 n = calc_normal(p);
    float sh = shadow(p + n * 0.01, L);           // 影は本物の太陽で落とす
    vec3 gi = mix(sky_hor, sky_up, n.y * 0.5 + 0.5) * F.p[1].z;

    vec3 col;
    if (mat <= 1 || mat == 7) {
        vec3 albedo = mat == 0 ? mix(vec3(0.20, 0.25, 0.50), vec3(0.30, 0.35, 0.60),
                                     step(0.5, fract((floor(p.x) + floor(p.z)) * 0.5)))
                    : vec3(0.75, 0.72, 0.70);
        col = albedo * (sun * max(dot(n, L), 0.0) * sh + gi);
    } else {
        vec3 albedo = mat == 2 ? vec3(1.0, 0.80, 0.70) : mat == 3 ? vec3(0.28, 0.16, 0.12)
                    : mat == 4 ? vec3(0.15, 0.28, 0.85) : mat == 5 ? vec3(0.08, 0.08, 0.12)
                    : vec3(0.15, 0.10, 0.35);
        // ---- タレント専用ライトの向き ----
        int mode = int(F.p[0].x + 0.5);
        vec3 Lt = L;
        if (mode == 1) Lt = rot_y(L, radians(F.p[0].y));                       // 太陽からズラす
        if (mode == 2) Lt = normalize(vec3(sin(radians(F.p[0].z)), 0.8, -cos(radians(F.p[0].z)))); // 固定
        float ndl = dot(n, Lt);
        float ramp = smoothstep(-0.02, 0.06, ndl) * mix(0.25, 1.0, sh);         // トゥーン 2 階調
        vec3 light_col = max(sun, vec3(0.05));

        // ---- GI 補正: 空が青くても肌だけ色を戻す ----
        vec3 g = saturate_color(gi, F.p[1].w);
        if (flag(1)) {
            float skin = (mat == 2) ? 1.0 : 0.4;
            vec3 neutral = vec3(luminance(g)) * vec3(1.0, 0.93, 0.88);
            g = mix(g, neutral, F.p[2].x * skin) * F.p[2].y;
        }
        col = albedo * (light_col * ramp + g);

        // ---- 輪郭リム: 太陽側の縁に出る。太陽が動けば出る面も回る ----
        if (flag(0)) {
            float fres = pow(1.0 - max(dot(n, -rd), 0.0), F.p[0].w);
            float side = max(dot(n, L), 0.0);                    // 太陽に向いた縁だけ
            float back = mix(0.35, 1.0, max(dot(rd, L), 0.0));  // 逆光ほど強く
            float in_shadow = mix(F.p[1].y, 1.0, sh);           // 影ではリムを減衰
            col += light_col * fres * side * back * in_shadow * F.p[1].x;
        }
    }
    float fog = 1.0 - exp(-t * 0.004);
    col = mix(col, sky_hor * 0.6, fog);
    o_color = finalize(col);
}
