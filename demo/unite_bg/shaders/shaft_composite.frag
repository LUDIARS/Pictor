#version 450
#extension GL_GOOGLE_include_directive : require
// =============================================================================
// Scene 3 / Pass B: ライトシャフト (光芒)
//   深度テクスチャ (Pass A の alpha = 距離) の上を、画面上の太陽位置に向かって
//   レイマーチする。各サンプルで「その先が空 (or 遠い) = 光が当たる空気」か
//   「建物で遮られている = 影の空気」かを判定し、当たっている割合を
//   大気散乱の色に掛けて足す。
//   p0: x=Sample Count  y=Distance Scale[m]  z=Intensity  w=Decay
//   p1: z=画面上のマーチ長 (太陽までの何割を見るか)
//   toggle Z: シャフト  X: 大気散乱の色を掛ける  C: 遮蔽バッファ表示  V: ジッター
// =============================================================================
#include "atmosphere.glsl"

layout(set = 0, binding = 7) uniform sampler2D u_scene;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 scene = texture(u_scene, v_uv);
    vec3 col = scene.rgb;
    vec3 rd = camera_ray(v_uv);

    float dist_scale = F.p[0].y;

    // 画面上の太陽位置
    vec4 sc = F.view_proj * vec4(F.cam_pos.xyz + F.sun_dir.xyz * 1e5, 1.0);
    bool sun_front = sc.w > 0.0;
    vec2 sun_uv = (sc.xy / sc.w) * 0.5 + 0.5;

    // 遮蔽バッファの表示: 白 = 光が当たる空気 (遠い/空), 黒 = 遮られる
    if (flag(2)) {
        float lit = scene.a >= dist_scale ? 1.0 : 0.0;
        vec3 dbg = mix(vec3(0.08), vec3(1.0, 0.85, 0.2), lit) * 0.6 + vec3(min(scene.a / 400.0, 1.0)) * 0.3;
        if (sun_front && length((v_uv - sun_uv) * vec2(F.resolution.x / F.resolution.y, 1.0)) < 0.02)
            dbg = vec3(1.0, 0.5, 0.0);
        o_color = vec4(dbg, 1.0);
        return;
    }

    if (flag(0) && sun_front && F.sun_color.w > 0.0) {
        int   N     = int(clamp(F.p[0].x, 2.0, 128.0));
        vec2  delta = (sun_uv - v_uv) * F.p[1].z / float(N);
        float jit   = flag(3) ? hash12(gl_FragCoord.xy + fract(F.cam_pos.w) * 61.0) : 0.5;
        float occ = 0.0, w = 1.0, wsum = 0.0;
        for (int i = 0; i < N; ++i) {
            vec2 suv = v_uv + delta * (float(i) + jit);
            if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) break;
            float d = texture(u_scene, suv).a;
            occ  += (d >= dist_scale ? 1.0 : 0.0) * w;
            wsum += w;
            w *= F.p[0].w;
        }
        occ = wsum > 0.0 ? occ / wsum : 0.0;

        // 太陽の方向を向くほど強い (ミー散乱の前方散乱)
        float mu = dot(rd, F.sun_dir.xyz);
        float phase = phase_mie(mu, F.atmos.z) * 4.0 * PI;
        vec3 shaft_col = flag(1) ? sky_color(rd, 8, 4) : F.sun_color.rgb * 0.2;
        col += shaft_col * occ * phase * F.p[0].z;
    }
    o_color = finalize(col);
}
