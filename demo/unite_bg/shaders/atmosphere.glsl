// =============================================================================
// atmosphere.glsl — 単一散乱による大気散乱 (レイリー + ミー)
//
// 視線レイを大気の中で数ステップに区切り、各サンプル点で
//   (1) その点に届く太陽光 = 太陽からその点までの減衰 (光学的距離 → exp)
//   (2) その点で視線方向へ散乱される割合 = 散乱係数 x 位相関数
//   (3) その点から目までの減衰
// を掛け合わせて足し込む。Profile の Rayleigh/Mie Color は「計算結果」に掛ける。
// =============================================================================
#ifndef UBG_ATMOSPHERE_GLSL
#define UBG_ATMOSPHERE_GLSL

#include "common.glsl"

const float R_EARTH = 6360e3;
const float R_ATMOS = 6420e3;

// レイリー散乱係数: 波長の 4 乗に反比例 (青ほど強く散乱)。
// 680nm で 5.8e-6 /m になるように正規化 → 550nm ≈ 13.6e-6, 450nm ≈ 30e-6
vec3 rayleigh_beta() {
    vec3 lambda = max(F.wavelength.xyz, vec3(1.0));
    return 5.8e-6 * pow(vec3(680.0) / lambda, vec3(4.0)) * F.wavelength.w;
}
// ミー散乱係数: 波長にほぼ依存しない (灰色)
float mie_beta() { return 21e-6 * F.atmos2.x; }

// 位相関数: 「入射方向に対してどの向きへどれだけ散る か」
float phase_rayleigh(float mu) { return 3.0 / (16.0 * PI) * (1.0 + mu * mu); }
// Cornette-Shanks (Henyey-Greenstein の改良)。g > 0 で前方散乱が強くなる
float phase_mie(float mu, float g) {
    float g2 = g * g;
    float num = 3.0 * (1.0 - g2) * (1.0 + mu * mu);
    float den = 8.0 * PI * (2.0 + g2) * pow(max(1.0 + g2 - 2.0 * g * mu, 1e-4), 1.5);
    return num / den;
}

// 球 (中心 = 原点) とレイの交差。戻り値 (t0, t1)。交差しなければ t1 < 0
vec2 ray_sphere(vec3 ro, vec3 rd, float r) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - r * r;
    float d = b * b - c;
    if (d < 0.0) return vec2(-1.0, -1.0);
    d = sqrt(d);
    return vec2(-b - d, -b + d);
}

struct AtmosResult {
    vec3 rayleigh;   // レイリー成分 (Tint 適用前)
    vec3 mie;        // ミー成分 (Tint 適用前)
    vec3 transmit;   // 視線方向の透過率 (遠景をどれだけ残すか)
};

// ro_m: 地表からの高さ [m] を含むワールド位置 (y を高度として使う)
// rd: 視線方向, L: 光源方向, max_dist: 地物までの距離 (空なら大きな値)
AtmosResult atmosphere_raw(vec3 ro_m, vec3 rd, vec3 L, float max_dist, int view_steps, int light_steps) {
    AtmosResult res;
    res.rayleigh = vec3(0.0); res.mie = vec3(0.0); res.transmit = vec3(1.0);

    vec3 ro = vec3(0.0, R_EARTH + max(ro_m.y, 1.0), 0.0);
    vec2 ta = ray_sphere(ro, rd, R_ATMOS);
    if (ta.y < 0.0) return res;
    float t_end = ta.y;
    vec2 tg = ray_sphere(ro, rd, R_EARTH);
    if (tg.x > 0.0) t_end = min(t_end, tg.x);   // 地平線の下 = 地面で止まる
    t_end = min(t_end, max_dist);

    float Hr = F.atmos.x, Hm = F.atmos.y;
    vec3  bR = rayleigh_beta();
    float bM = mie_beta();

    float seg = t_end / float(view_steps);
    float odR = 0.0, odM = 0.0;            // 目からサンプル点までの光学的距離
    vec3 sumR = vec3(0.0), sumM = vec3(0.0);

    for (int i = 0; i < view_steps; ++i) {
        vec3 p = ro + rd * (seg * (float(i) + 0.5));
        float h = length(p) - R_EARTH;
        float dR = exp(-h / Hr) * seg;
        float dM = exp(-h / Hm) * seg;
        odR += dR; odM += dM;

        // サンプル点 → 光源 方向の光学的距離
        vec2 tl = ray_sphere(p, L, R_ATMOS);
        float lseg = tl.y / float(light_steps);
        float lodR = 0.0, lodM = 0.0;
        bool blocked = false;
        for (int j = 0; j < light_steps; ++j) {
            vec3 q = p + L * (lseg * (float(j) + 0.5));
            float hq = length(q) - R_EARTH;
            if (hq < 0.0) { blocked = true; break; }   // 地球の影
            lodR += exp(-hq / Hr) * lseg;
            lodM += exp(-hq / Hm) * lseg;
        }
        if (blocked) continue;
        // Mie の消散は散乱の約 1.1 倍 (吸収込み)
        vec3 tau = bR * (odR + lodR) + bM * 1.1 * (odM + lodM);
        vec3 att = exp(-tau);
        sumR += att * dR;
        sumM += att * dM;
    }
    float mu = dot(rd, L);
    res.rayleigh = sumR * bR * phase_rayleigh(mu);
    res.mie      = sumM * bM * phase_mie(mu, F.atmos.z);
    res.transmit = exp(-(bR * odR + bM * 1.1 * odM));
    return res;
}

// Profile の Tint と強度を適用した最終的な空の色 (太陽 + 月)
vec3 sky_color_ex(vec3 ro_m, vec3 rd, float max_dist, int vs, int ls, out vec3 transmit) {
    vec3 col = vec3(0.0);
    transmit = vec3(1.0);
    float sun_i = F.atmos.w * F.sun_color.w;
    if (sun_i > 1e-4) {
        AtmosResult a = atmosphere_raw(ro_m, rd, F.sun_dir.xyz, max_dist, vs, ls);
        col += (a.rayleigh * F.rayleigh_tint.rgb + a.mie * F.mie_tint.rgb) * sun_i;
        transmit = a.transmit;
    }
    float moon_i = F.atmos.w * F.moon_dir.w;
    if (moon_i > 1e-4) {
        AtmosResult m = atmosphere_raw(ro_m, rd, F.moon_dir.xyz, max_dist, max(vs / 2, 4), max(ls / 2, 2));
        col += (m.rayleigh + m.mie) * F.moon_color.rgb * moon_i;
        if (sun_i <= 1e-4) transmit = m.transmit;
    }
    return col;
}
vec3 sky_color(vec3 rd, int vs, int ls) {
    vec3 t;
    return sky_color_ex(F.cam_pos.xyz, rd, 1e9, vs, ls, t);
}

// 太陽・月の円盤
vec3 sun_disk(vec3 rd) {
    float d = dot(rd, F.sun_dir.xyz);
    float s = smoothstep(0.99985, 0.99995, d) * 40.0 * F.sun_color.w;
    float m = smoothstep(0.99975, 0.99985, dot(rd, F.moon_dir.xyz)) * 2.0 * F.moon_dir.w;
    return s * F.sun_color.rgb + m * F.moon_color.rgb;
}

// 地物の照明に使う「その時刻の太陽光」。
// 色と強度はデザイナーが Profile (Sunlight Color / Intensity) で決めた値を使う
// (物理的な減衰を掛けると夕方に赤くなりすぎ、狙った見た目を作りにくいため)。
vec3 sun_light_at_ground() {
    vec3 L = F.sun_dir.xyz;
    return F.sun_color.rgb * F.sun_color.w * smoothstep(-0.06, 0.02, L.y);
}

// 空からの環境光 (GlobalGI の簡易版): 上向き・水平の空の色
vec3 sky_ambient_up()      { return sky_color(normalize(vec3(0.0, 1.0, 0.001)), 8, 4) * PI * 0.5; }
vec3 sky_ambient_horizon() {
    vec3 d = normalize(vec3(F.sun_dir.x, 0.12, F.sun_dir.z) * vec3(-1.0, 1.0, -1.0));
    return sky_color(d, 8, 4) * PI * 0.5;
}

#endif
