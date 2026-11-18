#version 450
#extension GL_GOOGLE_include_directive : require
// =============================================================================
// Scene 4: 海
//   p0: x=Amplitude  y=Steepness(Q)  z=Wavelength Scale  w=Wind Dir(deg)
//   p1: x=Shallow Depth[m]  y=Absorption  z=Deep Floor  w=Caustics Strength
//   p2: x=Foam  y=Wave Fade Start[m]  z=Wave Fade End[m]  w=Caustics Scale
//   objs[0] = 船の位置(xyz) + yaw,  objs[1] = pitch, roll  (CPU 側で同じ波を計算 = 浮力)
//   toggle Z: 浅瀬で波を弱める   X: 水底の色を抑える   C: 船の水を抜く(マスク)
//          V: LOD タイル表示    N: コースティクスを太陽光に追従   M: 法線で減衰
// =============================================================================
#include "atmosphere.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

#define NW 5
const float G = 9.81;

// ---------------------------------------------------------------- 海底 / 地形
float seabed(vec2 xz) {
    float island = smoothstep(90.0, 15.0, length(xz - vec2(55.0, 45.0)));
    float reef   = smoothstep(30.0, 5.0, length(xz - vec2(-25.0, 25.0))) * 6.0;
    return -13.0 + island * 15.0 + reef + (fbm2(xz * 0.05, 4) - 0.5) * 2.5;
}

// ---------------------------------------------------------------- Gerstner 波
void wave_params(int i, out vec2 D, out float A, out float L) {
    float offs[NW] = float[](0.0, 0.55, -0.45, 1.1, -1.25);
    float lens[NW] = float[](30.0, 17.0, 11.0, 7.0, 4.3);
    float ang = radians(F.p[0].w) + offs[i];
    D = vec2(cos(ang), sin(ang));
    L = lens[i] * F.p[0].z;
    A = L * 0.010 * F.p[0].x;
}
// 振幅の倍率: 浅いほど小さく (海底の深度から) × 遠いほど平面へ
float wave_scale(vec2 x0) {
    float s = 1.0;
    if (flag(0)) s *= clamp((0.0 - seabed(x0)) / max(F.p[1].x, 0.01), 0.0, 1.0);
    float d = length(x0 - F.cam_pos.xz);
    s *= 1.0 - smoothstep(F.p[2].y, F.p[2].z, d);
    return s;
}
// 格子点 x0 を Gerstner 波で動かした位置 (横方向にも動く = 波頭が尖る)
vec3 gerstner(vec2 x0, float t) {
    vec3 p = vec3(x0.x, 0.0, x0.y);
    float sc = wave_scale(x0);
    for (int i = 0; i < NW; ++i) {
        vec2 D; float A, L;
        wave_params(i, D, A, L);
        A *= sc;
        float k = 2.0 * PI / L;
        float w = sqrt(G * k);
        float th = k * dot(D, x0) - w * t;
        float Q = F.p[0].y / max(k * A * float(NW), 1e-4) * step(1e-5, A);
        Q = min(Q, 1.0 / max(k * A * float(NW), 1e-4));
        p.xz += Q * A * D * cos(th);
        p.y  += A * sin(th);
    }
    return p;
}
// ワールド xz の水面の高さ: 横ずれを打ち消すように格子点を逆算する
float water_height(vec2 xz) {
    float t = F.cam_pos.w;
    vec2 x0 = xz;
    for (int i = 0; i < 3; ++i) {
        vec3 p = gerstner(x0, t);
        x0 -= (p.xz - xz);
    }
    return gerstner(x0, t).y;
}
float max_amp() { return 0.010 * F.p[0].x * F.p[0].z * (30.0 + 17.0 + 11.0 + 7.0 + 4.3); }

// ---------------------------------------------------------------- 船 (SDF)
mat3 boat_rot() {
    float y = F.objs[0].w, pi = F.objs[1].x, r = F.objs[1].y;
    mat3 Ry = mat3(cos(y), 0, -sin(y),  0, 1, 0,  sin(y), 0, cos(y));
    mat3 Rx = mat3(1, 0, 0,  0, cos(pi), sin(pi),  0, -sin(pi), cos(pi));
    mat3 Rz = mat3(cos(r), sin(r), 0,  -sin(r), cos(r), 0,  0, 0, 1);
    return Ry * Rx * Rz;
}
vec3 to_boat(vec3 p) { return transpose(boat_rot()) * (p - F.objs[0].xyz); }
float sd_boat(vec3 p) {
    vec3 q = to_boat(p);
    float outer = sd_round_box(q, vec3(1.0, 0.5, 2.3), 0.35);
    float inner = sd_round_box(q - vec3(0.0, 0.25, 0.0), vec3(0.82, 0.55, 2.1), 0.25);
    return max(outer, -inner);
}
// ステンシル相当のボリューム: 船の内側に入った水面は描かない
bool inside_boat_volume(vec3 p) {
    vec3 q = to_boat(p) - vec3(0.0, 0.25, 0.0);
    return sd_box(q, vec3(0.84, 0.6, 2.12)) < 0.0;
}

// ---------------------------------------------------------------- 不透明物 SDF
// mat: 0 = 海底/島, 1 = 桟橋の柱, 2 = 桟橋の床, 3 = 船
float map(vec3 p, out int mat) {
    float d = (p.y - seabed(p.xz)) * 0.45; mat = 0;
    vec3 q = p - vec3(-8.0, 0.0, 0.0);
    q.z = q.z - 6.0 * clamp(round(q.z / 6.0), 0.0, 7.0);
    float pil = sd_cyl_y(q - vec3(0.0, -4.0, 0.0), 0.35, 5.6);
    if (pil < d) { d = pil; mat = 1; }
    float deck = sd_box(p - vec3(-8.0, 1.6, 21.0), vec3(1.6, 0.12, 22.0));
    if (deck < d) { d = deck; mat = 2; }
    float b = sd_boat(p);
    if (b < d) { d = b; mat = 3; }
    return d;
}
float map_d(vec3 p) { int m; return map(p, m); }
vec3 calc_normal(vec3 p) {
    const vec2 e = vec2(0.02, 0.0);
    return normalize(vec3(map_d(p + e.xyy) - map_d(p - e.xyy),
                          map_d(p + e.yxy) - map_d(p - e.yxy),
                          map_d(p + e.yyx) - map_d(p - e.yyx)));
}
float march(vec3 ro, vec3 rd, float tmax, out int mat) {
    float t = 0.0; mat = -1;
    for (int i = 0; i < 200; ++i) {
        int m;
        float h = map(ro + rd * t, m);
        if (h < 0.002 + 0.0015 * t) { mat = m; return t; }
        t += h;
        if (t > tmax) break;
    }
    return -1.0;
}

// 水面とレイの交差 (高さ場をステップ → 二分探索)
float water_hit(vec3 ro, vec3 rd) {
    float amax = max_amp() + 0.05;
    if (ro.y > amax && rd.y >= 0.0) return -1.0;
    float t0 = ro.y > amax ? (ro.y - amax) / -rd.y : 0.0;
    float t1 = rd.y < 0.0 ? min((ro.y + amax) / -rd.y, 1500.0) : 1500.0;
    float tp = t0;
    vec3 p = ro + rd * t0;
    float fp = p.y - water_height(p.xz);
    if (fp < 0.0) return t0;
    float t = t0;
    for (int i = 0; i < 96; ++i) {
        t = min(t + max(fp * 0.6, 0.03 + t * 0.006), t1);
        p = ro + rd * t;
        float f = p.y - water_height(p.xz);
        if (f < 0.0) {
            float a = tp, b = t;
            for (int k = 0; k < 6; ++k) {
                float m = 0.5 * (a + b);
                vec3 pm = ro + rd * m;
                if (pm.y - water_height(pm.xz) < 0.0) b = m; else a = m;
            }
            return 0.5 * (a + b);
        }
        tp = t; fp = f;
        if (t >= t1) break;
    }
    return (rd.y < 0.0) ? -ro.y / rd.y : -1.0;   // 遠方は平面
}
vec3 water_normal(vec2 xz, float t) {
    float e = clamp(0.02 * t, 0.05, 1.5);
    float h  = water_height(xz);
    float hx = water_height(xz + vec2(e, 0.0));
    float hz = water_height(xz + vec2(0.0, e));
    return normalize(vec3(h - hx, e, h - hz));
}

// ---------------------------------------------------------------- コースティクス
float caustic_pattern(vec2 p, float t) {
    float r1 = 1.0 - abs(value_noise2(p + vec2(t * 0.35, t * 0.2)) * 2.0 - 1.0);
    float r2 = 1.0 - abs(value_noise2(p * 1.37 + vec2(-t * 0.27, t * 0.31) + 5.3) * 2.0 - 1.0);
    return pow(r1 * r2, 7.0) * 4.0;
}

// 水中の面 (海底・柱・船体) の色
vec3 shade_underwater(vec3 p, vec3 n, int mat, vec3 sun) {
    vec3 L = F.sun_dir.xyz;
    vec3 albedo = mat == 0 ? mix(vec3(0.55, 0.50, 0.36), vec3(0.75, 0.70, 0.52), fbm2(p.xz * 0.3, 3))
                : mat == 3 ? vec3(0.75, 0.20, 0.25) : vec3(0.35, 0.30, 0.25);
    float depth = max(-p.y, 0.0);
    vec3 sigma = vec3(0.30, 0.07, 0.04) * F.p[1].y;
    vec3 light_down = exp(-sigma * depth / max(L.y, 0.2));       // 水面からここまでの減衰
    float ndl = max(dot(n, L), 0.0);
    vec3 col = albedo * (sun * ndl * light_down + sky_ambient_up() * 0.25 * exp(-sigma * depth));

    // コースティクス: 強い光 x 屈折させる水面 x 投影される面 の 3 条件
    float c = caustic_pattern(p.xz * F.p[2].w + p.y * 0.3, F.cam_pos.w);
    vec3 c_col  = flag(4) ? sun : vec3(1.0);                    // 太陽光の色・強さに追従
    float c_att = flag(5) ? smoothstep(0.0, 0.6, dot(n, L)) : 1.0; // 面の法線で減衰
    col += albedo * c * F.p[1].w * c_col * c_att * light_down * smoothstep(0.0, 0.4, depth);
    return col;
}

vec3 shade_above(vec3 p, vec3 n, int mat, vec3 sun) {
    vec3 L = F.sun_dir.xyz;
    vec3 albedo = mat == 0 ? mix(vec3(0.80, 0.72, 0.52), vec3(0.25, 0.45, 0.18), smoothstep(1.2, 2.2, p.y))
                : mat == 2 ? vec3(0.55, 0.45, 0.35)
                : mat == 3 ? vec3(0.85, 0.85, 0.9) : vec3(0.4, 0.35, 0.3);
    if (mat == 3 && to_boat(p).y < 0.05) albedo = vec3(0.75, 0.20, 0.25);
    return albedo * (sun * max(dot(n, L), 0.0) + sky_ambient_up() * (0.4 + 0.3 * n.y));
}

void main() {
    vec3 ro = F.cam_pos.xyz;
    vec3 rd = camera_ray(v_uv);
    vec3 sun = sun_light_at_ground();
    vec3 L = F.sun_dir.xyz;

    int smat;
    float ts = march(ro, rd, 900.0, smat);
    float tw = water_hit(ro, rd);
    vec3 col;

    bool water = tw > 0.0 && (ts < 0.0 || tw < ts);
    if (water && flag(2) && inside_boat_volume(ro + rd * tw)) water = false;  // 船の中の水を抜く
    if (water && ro.y < water_height(ro.xz)) water = false;

    if (water) {
        vec3 p = ro + rd * tw;
        vec3 n = water_normal(p.xz, tw);
        float cos_t = max(dot(-rd, n), 0.0);
        float fres = 0.02 + 0.98 * pow(1.0 - cos_t, 5.0);

        // 反射: 空の色 + 太陽のハイライト
        vec3 R = reflect(rd, n);
        R.y = abs(R.y);
        vec3 refl = sky_color(R, 8, 4) + sun * pow(max(dot(R, L), 0.0), 900.0) * 60.0;

        // 屈折: 水中へ進むレイで海底などを探す
        vec3 T = refract(rd, n, 1.0 / 1.333);
        int um;
        float tu = march(p + T * 0.05, T, 80.0, um);
        float path = tu > 0.0 ? tu : 80.0;
        vec3 sigma = vec3(0.30, 0.07, 0.04) * F.p[1].y;
        vec3 trans = exp(-sigma * path);
        vec3 under = vec3(0.0);
        float bed_y = -30.0;
        if (tu > 0.0) {
            vec3 q = p + T * tu;
            bed_y = q.y;
            under = shade_underwater(q, calc_normal(q), um, sun);
        }
        // 水の体積が散乱する色。水底の高さでブレンドすると深場が暗くなりすぎない
        vec3 amb = sun * max(L.y, 0.0) * 0.6 + sky_ambient_up() * 0.5;
        vec3 body;
        if (flag(1)) {
            float h = clamp((bed_y + 12.0) / 10.0, 0.0, 1.0);
            body = mix(vec3(0.02, 0.10, 0.22) * (0.4 + F.p[1].z), vec3(0.05, 0.35, 0.40), h) * amb;
        } else {
            body = vec3(0.002, 0.012, 0.025) * amb;
        }
        vec3 refr = under * trans + body * (1.0 - trans);

        col = mix(refr, refl, fres);

        // 泡: 岸 (浅い) と 波頭
        float depth = -seabed(p.xz);
        float shore = 1.0 - smoothstep(0.0, 1.2, depth);
        float crest = smoothstep(0.55, 0.9, p.y / max(max_amp(), 1e-3));
        float fn = fbm2(p.xz * 1.5 + F.cam_pos.w * 0.3, 3);
        float foam = clamp((shore + crest * 0.6) * smoothstep(0.45, 0.7, fn) * F.p[2].x, 0.0, 1.0);
        col = mix(col, (sun * max(L.y, 0.0) + sky_ambient_up()) * 0.8, foam);

        // LOD タイル表示: 近いほど細かいタイル、フェード後は平面
        if (flag(3)) {
            float d = length(p.xz - ro.xz);
            float lvl = clamp(floor(log2(max(d, 10.0) / 10.0)), 0.0, 3.0);
            float cell = 20.0 / exp2(3.0 - lvl);
            vec2 g = abs(fract(p.xz / cell + 0.5) - 0.5) * cell;
            float line = 1.0 - smoothstep(0.0, 0.04 * cell, min(g.x, g.y));
            vec3 lc = lvl < 0.5 ? vec3(1, 0.3, 0.3) : lvl < 1.5 ? vec3(1, 0.8, 0.2)
                    : lvl < 2.5 ? vec3(0.3, 1, 0.4) : vec3(0.3, 0.6, 1);
            if (d > F.p[2].z) lc = vec3(0.8);   // 平面
            col = mix(col, lc * 0.6, line * 0.8);
        }
    } else if (ts > 0.0) {
        vec3 p = ro + rd * ts;
        vec3 n = calc_normal(p);
        bool underwater = p.y < water_height(p.xz) - 0.02;
        col = underwater ? shade_underwater(p, n, smat, sun) : shade_above(p, n, smat, sun);
        // 船の中を見た時 (マスク OFF なら水面が見えるはずの所) の水: ここは船内の床
    } else {
        col = sky_color(rd, 12, 6) + sun_disk(rd);
    }
    o_color = finalize(col);
}
