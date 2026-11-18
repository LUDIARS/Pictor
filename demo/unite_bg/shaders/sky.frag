#version 450
#extension GL_GOOGLE_include_directive : require
// =============================================================================
// Scene 1: 大気散乱 (空)
//   toggle Z: Rayleigh  X: Mie  C: Tint(Profile の色を掛ける)  V: 地面
//   調整値は全シーン共通の Atmosphere 設定 (HUD 1 番)
// =============================================================================
#include "atmosphere.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec3 rd = camera_ray(v_uv);
    vec3 ro = F.cam_pos.xyz;
    bool use_r = flag(0), use_m = flag(1), use_tint = flag(2), show_ground = flag(3);

    // 地面 (y = 0) に当たるか
    float t_ground = (rd.y < -1e-4) ? -ro.y / rd.y : 1e9;
    float max_dist = show_ground ? t_ground : 1e9;

    vec3 col = vec3(0.0);
    float sun_i = F.atmos.w * F.sun_color.w;
    AtmosResult a = atmosphere_raw(ro, rd, F.sun_dir.xyz, max_dist, 16, 8);
    vec3 rt = use_tint ? F.rayleigh_tint.rgb : vec3(1.0);
    vec3 mt = use_tint ? F.mie_tint.rgb : vec3(1.0);
    if (use_r) col += a.rayleigh * rt * sun_i;
    if (use_m) col += a.mie * mt * sun_i;

    // 月による散乱 (夜でも真っ黒にしない)
    if (F.moon_dir.w > 1e-4) {
        AtmosResult m = atmosphere_raw(ro, rd, F.moon_dir.xyz, max_dist, 8, 4);
        vec3 mc = vec3(0.0);
        if (use_r) mc += m.rayleigh;
        if (use_m) mc += m.mie;
        col += mc * F.moon_color.rgb * F.atmos.w * F.moon_dir.w;
    }

    if (show_ground && t_ground < 1e8) {
        // 地面: 太陽光 x アルベド。遠いほど空気の色に近づく (= 空気遠近、これも散乱)
        vec3 p = ro + rd * t_ground;
        float checker = mod(floor(p.x / 50.0) + floor(p.z / 50.0), 2.0);
        vec3 albedo = mix(vec3(0.10, 0.16, 0.08), vec3(0.14, 0.20, 0.10), checker);
        vec3 lit = albedo * (sun_light_at_ground() * max(F.sun_dir.y, 0.0) + sky_ambient_up() * 0.3);
        col += lit * a.transmit;
    } else {
        col += sun_disk(rd) * a.transmit;
    }
    o_color = finalize(col);
}
