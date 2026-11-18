#version 450
#extension GL_GOOGLE_include_directive : require
// =============================================================================
// 雲の SixWay ベイク (起動時に 1 回 / B キーで再ベイク)
//   板ポリ 1 枚分の雲を「厚みのある体積」としてレイマーチし、
//   6 方向それぞれから光を当てた時の明るさをテクスチャのチャンネルに焼く。
//   Positive: R = 右(+X) から, G = 上(+Y) から, B = 奥(+Z) から, A = 不透明度
//   Negative: R = 左(-X) から, G = 下(-Y) から, B = 手前(-Z) から, A = 不透明度
//   ※ ここでの「方向」は光が来る方向 (= 面から光源へのベクトル)
// =============================================================================
#include "common.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_pos;
layout(location = 1) out vec4 o_neg;

const float SIGMA = 16.0;     // 消散係数 (大きいほど濃い雲)
const float HALF_Z = 0.35;    // 雲の厚み (半分)

float density(vec3 p) {
    // もこもこの塊 (球の集まり) + 平らな底 + ノイズで削る
    float shape = 0.0;
    for (int i = 0; i < 7; ++i) {
        float fi = float(i);
        vec3 c = vec3(-0.65 + fi * 0.22, -0.12 + 0.18 * sin(fi * 2.1) * (1.0 - abs(fi - 3.0) / 3.5), 0.05 * cos(fi * 1.3));
        float r = 0.20 + 0.10 * sin(fi * 1.7 + 0.5) + 0.12 * (1.0 - abs(fi - 3.0) / 3.0);
        shape = max(shape, 1.0 - length((p - c) / vec3(r, r * 0.85, r * 1.2)));
    }
    shape *= smoothstep(-0.30, -0.18, p.y);
    float erode = fbm3(p * 7.0 + 3.0, 5);
    return clamp((shape - erode * 0.45) * 4.0, 0.0, 1.0);
}

float light_transmit(vec3 p, vec3 dir) {
    float od = 0.0;
    const int N = 10;
    const float ds = 0.05;
    for (int i = 1; i <= N; ++i) od += density(p + dir * ds * float(i)) * ds;
    float t = exp(-SIGMA * od);
    // 多重散乱の近似: 弱く減衰する成分を混ぜて、厚い所も真っ黒にしない
    return max(t, 0.3 * exp(-SIGMA * 0.25 * od));
}

void main() {
    // テクスチャ (2:1) → 雲のローカル座標 x:[-1,1], y:[-0.5,0.5] (上が +)
    vec3 base = vec3(v_uv.x * 2.0 - 1.0, 0.5 - v_uv.y, 0.0);
    const vec3 dirs[6] = vec3[](vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, 0, 1),
                                vec3(-1, 0, 0), vec3(0, -1, 0), vec3(0, 0, -1));
    float acc[6] = float[](0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    float T = 1.0;
    const int VS = 28;
    float dz = 2.0 * HALF_Z / float(VS);
    // カメラ (手前 = -Z) 側から奥へ向かって積分
    for (int s = 0; s < VS; ++s) {
        vec3 p = base + vec3(0.0, 0.0, -HALF_Z + dz * (float(s) + 0.5));
        float d = density(p);
        if (d < 1e-3) continue;
        float a = SIGMA * d * dz;
        for (int k = 0; k < 6; ++k) acc[k] += T * a * light_transmit(p, dirs[k]);
        T *= exp(-a);
        if (T < 0.01) break;
    }
    float alpha = 1.0 - T;
    o_pos = vec4(acc[0], acc[1], acc[2], alpha);
    o_neg = vec4(acc[3], acc[4], acc[5], alpha);
}
