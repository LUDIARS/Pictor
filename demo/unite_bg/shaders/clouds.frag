#version 450
#extension GL_GOOGLE_include_directive : require
// =============================================================================
// Scene 5: 雲 (SixWay ライティング + 前方散乱 + 雲専用ライト)
//   p0: x=Cloud Light Intensity  y=Forward Scatter  z=Forward Power  w=Ambient
//   p1: x=Cloud Light Hue  y=Cloud Light Saturation   (色は C++ が p2.rgb に変換)
//   objs[0..11] = 雲 (xyz = 中心, w = 大きさ[m]) 遠い順,  objs[12].x = 数
//   toggle Z: SixWay (OFF = 一様な明るさ)  X: 前方散乱  C: 雲専用ライト  V: ベイク結果を表示
// =============================================================================
#include "atmosphere.glsl"

layout(set = 0, binding = 1) uniform sampler2D u_cloud_pos;
layout(set = 0, binding = 2) uniform sampler2D u_cloud_neg;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    if (flag(3)) {   // ベイク結果: 上 = Positive, 下 = Negative
        vec2 uv = vec2(v_uv.x, fract(v_uv.y * 2.0));
        vec4 t = v_uv.y < 0.5 ? texture(u_cloud_pos, uv) : texture(u_cloud_neg, uv);
        vec3 bc = t.rgb * 1.6;
        if (F.misc.x > 0.5) bc = pow(bc, vec3(1.0 / 2.2));
        o_color = vec4(bc, 1.0);
        return;
    }

    vec3 ro = F.cam_pos.xyz;
    vec3 rd = camera_ray(v_uv);
    vec3 L = F.sun_dir.xyz;

    vec3 col = sky_color(rd, 12, 6) + sun_disk(rd);
    // 海の地平線 (雲が浮かぶ位置の目安)
    if (rd.y < 0.0) col = mix(col * 0.25, vec3(0.01, 0.03, 0.06) * (F.sun_color.w + 0.1), 0.6);

    // 雲に当てる光: 雲専用ライト (色・強度だけ別) か シーンの太陽光か。向きはどちらも太陽
    vec3 light = flag(2) ? F.p[2].rgb * F.p[0].x : sun_light_at_ground() * 2.0;
    vec3 ambient = sky_ambient_up() * F.p[0].w;

    int count = int(F.objs[12].x);
    for (int i = 0; i < count; ++i) {
        vec3 c = F.objs[i].xyz;
        float size = F.objs[i].w;
        vec3 nrm = normalize(ro - c);                    // カメラを向く板ポリ
        float den = dot(rd, nrm);
        if (abs(den) < 1e-4) continue;
        float t = dot(c - ro, nrm) / den;
        if (t <= 0.0) continue;
        vec3 hp = ro + rd * t;
        vec3 right = normalize(cross(vec3(0.0, 1.0, 0.0), nrm));
        vec3 up = cross(nrm, right);
        vec2 lp = vec2(dot(hp - c, right), dot(hp - c, up)) / size;
        if (abs(lp.x) > 1.0 || abs(lp.y) > 0.5) continue;
        // 雲ごとに左右反転してバリエーション
        if ((i & 1) == 1) lp.x = -lp.x;
        vec2 uv = vec2(lp.x * 0.5 + 0.5, 0.5 - lp.y);

        vec4 P = texture(u_cloud_pos, uv);
        vec4 N = texture(u_cloud_neg, uv);
        float alpha = P.a;
        if (alpha < 1e-3) continue;

        // 光の方向を雲の板ポリの座標系 (右 / 上 / 奥) に直す
        vec3 lb = vec3(dot(L, right) * ((i & 1) == 1 ? -1.0 : 1.0), dot(L, up), dot(L, -nrm));
        vec3 w2 = lb * lb;   // 二乗の重み (合計 = 1)
        float lit;
        if (flag(0)) {
            lit = (lb.x > 0.0 ? P.r : N.r) * w2.x
                + (lb.y > 0.0 ? P.g : N.g) * w2.y
                + (lb.z > 0.0 ? P.b : N.b) * w2.z;
        } else {
            lit = alpha * 0.45;                            // 方向を見ない = のっぺり
        }
        vec3 rad = lit * light + alpha * ambient;

        // 前方散乱: 視線と太陽の向きが揃うほど明るく (縁ほど透けて光る)
        if (flag(1)) {
            float fw = pow(max(dot(rd, L), 0.0), F.p[0].z);
            rad += light * F.p[0].y * fw * alpha * (1.0 - 0.6 * alpha);
        }
        col = col * (1.0 - alpha) + rad;                  // 奥から順に合成
    }
    o_color = finalize(col);
}
