#version 450
#extension GL_GOOGLE_include_directive : require
// =============================================================================
// Scene 2: 夜空 (星・瞬き・天の川)
//   p0: x=Star Tiling  y=Star Exposure  z=Flicker Speed  w=Flicker Strength
//   p1: xyz=Star Rotation Axis  w=Rotation Daily Cycle (1 日に何回転)
//   p2: x=Rotation Angle Offset(deg)  y=Galaxy Exposure  z=Galaxy Width
//   toggle Z: 瞬き  X: 天の川  C: 分解表示(星/ノイズ/結果)  V: Profile カーブを使う
// =============================================================================
#include "atmosphere.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

mat3 axis_angle(vec3 axis, float ang) {
    axis = normalize(axis);
    float s = sin(ang), c = cos(ang), oc = 1.0 - c;
    return mat3(oc * axis.x * axis.x + c,          oc * axis.x * axis.y + axis.z * s, oc * axis.z * axis.x - axis.y * s,
                oc * axis.x * axis.y - axis.z * s, oc * axis.y * axis.y + c,          oc * axis.y * axis.z + axis.x * s,
                oc * axis.z * axis.x + axis.y * s, oc * axis.y * axis.z - axis.x * s, oc * axis.z * axis.z + c);
}

// 「星のテクスチャ」相当: 方向を 3D グリッドで区切り、セルごとに 1 個まで星を置く
float star_field(vec3 d, float tiling) {
    vec3 g = d * tiling;
    vec3 cell = floor(g);
    vec3 rnd = hash33(cell);
    if (rnd.x > 0.18) return 0.0;                 // 星がないセル
    vec3 star_pos = cell + 0.2 + 0.6 * hash33(cell + 7.1);
    float dist = length(g - star_pos);
    float size = mix(0.05, 0.14, rnd.y);
    float b = smoothstep(size, 0.0, dist);
    return b * mix(0.3, 1.6, rnd.z * rnd.z);
}

// 方向 → 正距円筒 UV (ノイズの UV スクロール用)
vec2 dir_to_uv(vec3 d) {
    return vec2(atan(d.z, d.x) / (2.0 * PI) + 0.5, asin(clamp(d.y, -1.0, 1.0)) / PI + 0.5);
}

void main() {
    vec3 rd = camera_ray(v_uv);
    float sky_time = F.sun_dir.w;

    // 1 日の回転: SkyTime x DailyCycle 回転 + オフセット
    float ang = (sky_time * F.p[1].w) * 2.0 * PI + radians(F.p[2].x);
    mat3 R = axis_angle(F.p[1].xyz, ang);
    vec3 sd = R * rd;

    float stars = star_field(sd, F.p[0].x);

    // 瞬き: 星とは別のノイズを UV スクロールして掛ける → 明るさが時間で揺らぐ
    vec2 uv = dir_to_uv(sd);
    float n = value_noise2(uv * vec2(400.0, 200.0) + vec2(F.cam_pos.w * F.p[0].z * 10.0, 0.0));
    float flicker = flag(0) ? mix(1.0, n * 1.8, F.p[0].w) : 1.0;

    // 天の川: 回転した空の中の大円に沿った帯
    vec3 gal_n = normalize(vec3(0.3, 0.2, 1.0));
    float b = dot(sd, gal_n);
    float band = exp(-pow(b / max(F.p[2].z, 0.01), 2.0));
    float dust = fbm3(sd * 6.0, 5);
    float glow = fbm3(sd * 2.5 + 3.0, 4);
    vec3 galaxy = band * (vec3(0.35, 0.40, 0.75) * glow + vec3(0.55, 0.45, 0.6) * pow(glow, 3.0))
                * smoothstep(0.25, 0.65, dust + 0.15);

    float star_k   = flag(3) ? F.atmos2.y : 1.0;   // Profile: Star Intensity カーブ
    float galaxy_k = flag(3) ? F.atmos2.z : 1.0;   // Profile: Galaxy Intensity カーブ

    vec3 col = sky_color(rd, 12, 6);
    vec3 night = vec3(stars * flicker) * F.p[0].y * star_k;
    if (flag(1)) night += galaxy * F.p[2].y * galaxy_k;

    // 地平線の下は丘のシルエット
    float hill = 0.02 + 0.03 * fbm2(vec2(atan(rd.z, rd.x) * 6.0, 0.0), 3);
    if (rd.y < hill) { night = vec3(0.0); col *= 0.15; }

    col += night * 0.05;
    col += sun_disk(rd);

    // 分解表示: 左 = 星のテクスチャ, 中 = ノイズ, 右 = 掛けた結果
    if (flag(2)) {
        float third = v_uv.x * 3.0;
        if (third < 1.0)      col = vec3(stars);
        else if (third < 2.0) col = vec3(n);
        else                  col = vec3(stars * flicker);
        o_color = vec4(col, 1.0);
        return;
    }
    o_color = finalize(col);
}
