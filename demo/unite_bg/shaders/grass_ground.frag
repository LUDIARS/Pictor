#version 450
#extension GL_GOOGLE_include_directive : require
// =============================================================================
// Scene 8 の背景: 空 + 地面 + 建物 + プレイヤー をレイマーチし、深度も書く。
// 深度を書くので、後から描く草のメッシュと前後関係が正しくなる。
//   objs[0] = 建物の中心 (xyz) + 状態(w: 0 なし / 1 建築中 / 2 完成)
//   objs[1] = 建物の半サイズ (xyz)
//   objs[2] = プレイヤー位置
// =============================================================================
#include "atmosphere.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

float map(vec3 p, out int mat) {
    float d = p.y; mat = 0;
    if (F.objs[0].w > 0.5) {
        vec3 c = F.objs[0].xyz, hs = F.objs[1].xyz;
        float b = sd_box(p - c - vec3(0.0, hs.y, 0.0), hs);
        if (F.objs[0].w < 1.5) {   // 建築中: 足場の骨組み
            vec3 q = p - c - vec3(0.0, hs.y, 0.0);
            float frame = max(b, -sd_box(q, hs - vec3(0.25, -0.1, 0.25)));
            frame = max(frame, -sd_box(q, vec3(hs.x + 1.0, hs.y - 0.3, hs.z - 0.3)));
            b = frame;
        }
        if (b < d) { d = b; mat = F.objs[0].w < 1.5 ? 2 : 1; }
    }
    float pl = sd_capsule(p, F.objs[2].xyz + vec3(0, 0.3, 0), F.objs[2].xyz + vec3(0, 1.4, 0), 0.3);
    if (flag(3) && pl < d) { d = pl; mat = 3; }
    return d;
}

void main() {
    vec3 ro = F.cam_pos.xyz;
    vec3 rd = camera_ray(v_uv);
    float t = 0.0; int mat = -1;
    for (int i = 0; i < 128; ++i) {
        int m;
        float h = map(ro + rd * t, m);
        if (h < 0.001 + 0.001 * t) { mat = m; break; }
        t += h;
        if (t > 800.0) break;
    }
    if (mat < 0) {
        gl_FragDepth = 1.0;
        o_color = finalize(sky_color(rd, 12, 6) + sun_disk(rd));
        return;
    }
    vec3 p = ro + rd * t;
    int dm;
    const vec2 e = vec2(0.01, 0.0);
    vec3 n = normalize(vec3(map(p + e.xyy, dm) - map(p - e.xyy, dm),
                            map(p + e.yxy, dm) - map(p - e.yxy, dm),
                            map(p + e.yyx, dm) - map(p - e.yyx, dm)));
    vec3 albedo = vec3(0.10, 0.16, 0.05);
    if (mat == 0) {
        float path = abs(p.z - 8.0 * sin(p.x * 0.05)) ;
        albedo = mix(vec3(0.35, 0.28, 0.20), albedo, smoothstep(1.5, 2.5, path));
    }
    if (mat == 1) albedo = vec3(0.80, 0.78, 0.85);
    if (mat == 2) albedo = vec3(0.55, 0.45, 0.30);
    if (mat == 3) albedo = vec3(0.9, 0.3, 0.3);
    vec3 col = albedo * (sun_light_at_ground() * max(dot(n, F.sun_dir.xyz), 0.0)
                       + sky_ambient_up() * (0.5 + 0.5 * n.y) * 0.5);
    col = mix(col, sky_ambient_horizon() * 0.7, 1.0 - exp(-t * 0.004));
    gl_FragDepth = world_to_depth(p);
    o_color = finalize(col);
}
