#version 450
#extension GL_GOOGLE_include_directive : require
// =============================================================================
// Scene 9: SkyTime と Profile
//   上半分: 今の SkyTime の空 (水平方向を見回したパノラマ)
//   下半分: Profile のトラック (左 0:00 → 右 24:00)
//     1 段目 Sunlight Color (グラデーション)  2 段目 Rayleigh Color
//     3 段目 Mie Color                         4 段目 Star Intensity (カーブ)
//   縦線 = 今の時刻。キーの位置に小さな印。
// =============================================================================
#include "atmosphere.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

// C++ 側 (sky_profile.cpp) と同じ補間をする
vec3 eval_gradient(int track, float t) {
    int n = F.profile_counts[track];
    if (n == 0) return vec3(0.0);
    vec4 first = F.profile_keys[track * 12];
    if (t <= first.a) return first.rgb;
    for (int i = 0; i < n - 1; ++i) {
        vec4 a = F.profile_keys[track * 12 + i], b = F.profile_keys[track * 12 + i + 1];
        if (t <= b.a) return mix(a.rgb, b.rgb, (t - a.a) / max(b.a - a.a, 1e-5));
    }
    return F.profile_keys[track * 12 + n - 1].rgb;
}
float eval_curve(int track, float t) {
    int n = F.profile_counts[track];
    if (n == 0) return 0.0;
    vec4 first = F.profile_keys[track * 12];
    if (t <= first.a) return first.r;
    for (int i = 0; i < n - 1; ++i) {
        vec4 a = F.profile_keys[track * 12 + i], b = F.profile_keys[track * 12 + i + 1];
        if (t <= b.a) return mix(a.r, b.r, smoothstep(0.0, 1.0, (t - a.a) / max(b.a - a.a, 1e-5)));
    }
    return F.profile_keys[track * 12 + n - 1].r;
}

void main() {
    float now = F.sun_dir.w;
    if (v_uv.y < 0.5) {
        // パノラマ: 横 = 方位 360°, 縦 = 仰角 -10°..60°
        float az = v_uv.x * 2.0 * PI;
        float el = mix(radians(60.0), radians(-10.0), v_uv.y / 0.5);
        vec3 rd = vec3(cos(az) * cos(el), sin(el), sin(az) * cos(el));
        vec3 c = el < 0.0 ? vec3(0.02, 0.03, 0.03) * (F.sun_color.w + 0.05)
                          : sky_color(rd, 12, 6) + sun_disk(rd);
        o_color = finalize(c);
        return;
    }
    float y = (v_uv.y - 0.5) / 0.5;        // 0..1
    int track = int(floor(y * 4.0));
    float ty = fract(y * 4.0);
    float t = v_uv.x;
    vec3 c;
    if (ty < 0.12 || ty > 0.92) {
        c = vec3(0.08);
    } else if (track < 3) {
        c = eval_gradient(track, t);
        c = c / (1.0 + c);                  // HDR の色を見やすく
    } else {
        float v = eval_curve(track, t) / 2.0;           // Y Max = 2
        float yy = 1.0 - (ty - 0.12) / 0.8;
        c = vec3(0.12);
        if (abs(yy - v) < 0.02) c = vec3(0.3, 1.0, 0.3);
        if (fract(t * 4.0) < 0.004) c = vec3(0.3);       // 6 時間ごとの目盛
    }
    // キーの印
    int n = F.profile_counts[min(track, 3)];
    for (int i = 0; i < n; ++i) {
        float kt = F.profile_keys[track * 12 + i].a;
        if (abs(t - kt) < 0.0025 && ty > 0.85) c = vec3(1.0);
    }
    // 今の時刻
    if (abs(t - now) < 0.0015) c = vec3(1.0, 0.6, 0.1);
    if (F.misc.x > 0.5) c = pow(c, vec3(1.0 / 2.2));
    o_color = vec4(c, 1.0);
}
