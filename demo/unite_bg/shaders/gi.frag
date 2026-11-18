#version 450
#extension GL_GOOGLE_include_directive : require
// =============================================================================
// Scene 6: 間接光 (GI) — 優先度: LocalGI > SkyOcclusion x GlobalGI > GlobalGI
//   p0: x=GI Intensity  y=Bulb Intensity  z=Local Blend Distance[m]  w=Probe View Size
//   toggle Z: SkyOcclusion  X: 地面側を上書き(GlobalGI)  C: LocalGI  V: プローブ表示
//          N: 動く箱 (ベイク後に出てくる物)  M: GI のみ表示 (直接光を消す)
// =============================================================================
#include "atmosphere.glsl"
#include "gi_scene.glsl"

layout(set = 0, binding = 3) uniform sampler2D u_probe_sh;
layout(set = 0, binding = 4) uniform sampler2D u_local_diffuse;
layout(set = 0, binding = 5) uniform sampler2D u_local_normal;
layout(set = 0, binding = 6) uniform sampler2D u_local_emission;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

// ---------------------------------------------------------------- GlobalGI
// 空の色から作る環境光。地面方向まで空の色だと「下向きが青すぎる」ので
// 下半球は地面の照り返しに近い色で上書きする。
vec3 g_sky_up, g_sky_hor, g_ground;
vec3 global_gi(vec3 n) {
    vec3 upper = mix(g_sky_hor, g_sky_up, clamp(n.y, 0.0, 1.0));
    if (n.y >= 0.0) return upper;
    vec3 lower = flag(1) ? g_ground : mix(g_sky_hor, g_sky_up, -n.y);
    return mix(g_sky_hor, lower, clamp(-n.y * 1.5, 0.0, 1.0));
}

// ---------------------------------------------------------------- SkyOcclusion
vec4 probe_fetch(ivec3 i) {
    i = clamp(i, ivec3(0), PROBE_COUNT - 1);
    return texelFetch(u_probe_sh, ivec2(i.x, i.y + i.z * PROBE_COUNT.y), 0);
}
// 8 個の近いプローブを 3 線形補間
vec4 probe_sample(vec3 p) {
    vec3 g = (p - PROBE_ORIGIN) / PROBE_SPACING;
    vec3 f = fract(g);
    ivec3 b = ivec3(floor(g));
    vec4 r = vec4(0.0);
    for (int k = 0; k < 8; ++k) {
        ivec3 o = ivec3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        vec3 w3 = mix(1.0 - f, f, vec3(o));
        r += probe_fetch(b + o) * (w3.x * w3.y * w3.z);
    }
    return r;
}
// L1 SH → 法線方向の「余弦で重み付けした空の見え具合」(0..1)
float sky_visibility(vec4 sh, vec3 n) {
    float e = PI * 0.282095 * sh.x + (2.0 * PI / 3.0) * 0.488603 * dot(sh.yzw, n);
    return clamp(e / PI, 0.0, 1.0);
}
bool in_probe_grid(vec3 p) {
    vec3 lo = PROBE_ORIGIN, hi = PROBE_ORIGIN + vec3(PROBE_COUNT - 1) * PROBE_SPACING;
    return all(greaterThanEqual(p, lo - 0.5)) && all(lessThanEqual(p, hi + 0.5));
}

// ---------------------------------------------------------------- LocalGI
vec3 g_local_dir_color[6];
void build_local_probe(vec3 sun) {
    const vec3 axes[6] = vec3[](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0),
                                vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));
    for (int i = 0; i < 6; ++i) {
        vec4 dif = texelFetch(u_local_diffuse, ivec2(i, 0), 0);
        vec3 nrm = texelFetch(u_local_normal, ivec2(i, 0), 0).xyz * 2.0 - 1.0;
        vec3 emi = texelFetch(u_local_emission, ivec2(i, 0), 0).rgb;
        // ベイクした材質を「今の」太陽と空でライティングし直す
        vec3 lit = dif.rgb * (sun * max(dot(nrm, F.sun_dir.xyz), 0.0) + g_sky_up * 0.35);
        // 空が見えていた割合は、今の空の色に差し替える
        vec3 sky = sky_color(axes[i], 6, 3) * PI * 0.5;
        g_local_dir_color[i] = lit * dif.a + sky * (1.0 - dif.a) + emi * F.p[0].y;
    }
}
// 6 方向の色から法線方向の照度を作る (Ambient Cube)
vec3 local_gi(vec3 n) {
    vec3 n2 = n * n;
    return n2.x * (n.x > 0.0 ? g_local_dir_color[0] : g_local_dir_color[1])
         + n2.y * (n.y > 0.0 ? g_local_dir_color[2] : g_local_dir_color[3])
         + n2.z * (n.z > 0.0 ? g_local_dir_color[4] : g_local_dir_color[5]);
}
float local_weight(vec3 p) {
    vec3 d = abs(p - LOCAL_CENTER) - LOCAL_HALF;
    float outside = max(max(d.x, d.y), d.z);
    return 1.0 - smoothstep(-max(F.p[0].z, 0.01), 0.0, outside);
}

// ---------------------------------------------------------------- 合成
vec3 evaluate_gi(vec3 p, vec3 n) {
    vec3 g = global_gi(n);
    if (flag(0) && in_probe_grid(p)) g *= sky_visibility(probe_sample(p + n * 0.2), n);
    if (flag(2)) g = mix(g, local_gi(n), local_weight(p));
    return g * F.p[0].x;
}

// プローブを球で可視化する (近いものだけ)
float probe_spheres(vec3 p, out vec3 center) {
    vec3 g = clamp(round((p - PROBE_ORIGIN) / PROBE_SPACING), vec3(0.0), vec3(PROBE_COUNT - 1));
    center = PROBE_ORIGIN + g * PROBE_SPACING;
    return length(p - center) - F.p[0].w;
}

float soft_shadow(vec3 ro, vec3 rd) {
    float res = 1.0, t = 0.03;
    for (int i = 0; i < 48 && t < 40.0; ++i) {
        int m;
        float h = gi_map(ro + rd * t, true, m);
        if (h < 0.001) return 0.0;
        res = min(res, 8.0 * h / t);
        t += clamp(h, 0.03, 2.0);
    }
    return clamp(res, 0.0, 1.0);
}

void main() {
    vec3 ro = F.cam_pos.xyz;
    vec3 rd = camera_ray(v_uv);
    vec3 sun = sun_light_at_ground();
    vec3 L = F.sun_dir.xyz;

    g_sky_up  = sky_ambient_up();
    g_sky_hor = sky_ambient_horizon();
    // 地面の照り返し: 地面の平均的な色 x (太陽 + 空)
    g_ground  = vec3(0.45, 0.42, 0.40) * (sun * max(L.y, 0.0) + g_sky_up * 0.5) * 0.5;
    if (flag(2)) build_local_probe(sun);

    int id;
    float t = gi_march(ro, rd, 200.0, true, id);

    // プローブ表示
    if (flag(3)) {
        float tp = 0.0; vec3 c = vec3(0.0); bool hitp = false;
        for (int i = 0; i < 96; ++i) {
            float h = probe_spheres(ro + rd * tp, c);
            if (h < 0.002) { hitp = true; break; }
            tp += h;
            if (tp > 60.0) break;
        }
        if (hitp && (t < 0.0 || tp < t)) {
            vec3 n = normalize(ro + rd * tp - c);
            vec3 g = global_gi(n);
            if (flag(0)) g *= sky_visibility(probe_fetch(ivec3(round((c - PROBE_ORIGIN) / PROBE_SPACING))), n);
            if (flag(2)) g = mix(g, local_gi(n), local_weight(c));
            o_color = finalize(g * F.p[0].x);
            return;
        }
    }

    vec3 col;
    if (t < 0.0) {
        col = sky_color(rd, 12, 6) + sun_disk(rd);
    } else {
        vec3 p = ro + rd * t;
        vec3 n = gi_normal(p, true);
        Mat m = gi_material(p, id);
        vec3 direct = flag(5) ? vec3(0.0) : sun * max(dot(n, L), 0.0) * soft_shadow(p + n * 0.02, L);
        vec3 indirect = evaluate_gi(p, n);
        col = m.albedo * (direct + indirect) + m.emission * F.p[0].y * 0.2;
    }
    o_color = finalize(col);
}
