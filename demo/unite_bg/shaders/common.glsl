// =============================================================================
// common.glsl — Unite BG demo 共通定義
//   ・フレーム UBO (C++ 側 FrameUBO と std140 で一致させる)
//   ・カメラレイ生成 / ハッシュ / ノイズ / トーンマップ
// =============================================================================
#ifndef UBG_COMMON_GLSL
#define UBG_COMMON_GLSL

#define PI 3.14159265359

layout(std140, set = 0, binding = 0) uniform FrameUBO {
    mat4  view_proj;
    mat4  inv_view_proj;
    vec4  cam_pos;        // xyz = カメラ位置 [m],  w = 経過秒
    vec4  sun_dir;        // xyz = 太陽への方向,     w = SkyTime (0..1 = 0:00..24:00)
    vec4  moon_dir;       // xyz = 月への方向,       w = 月光強度
    vec4  sun_color;      // rgb = 太陽光の色 (Profile),  w = 太陽光強度 (Profile)
    vec4  moon_color;     // rgb = 月光の色 (Profile)
    vec4  rayleigh_tint;  // rgb = レイリー計算結果に掛ける色 (Profile)
    vec4  mie_tint;       // rgb = ミー計算結果に掛ける色 (Profile)
    vec4  wavelength;     // xyz = R/G/B の代表波長 [nm], w = 分子密度倍率
    vec4  atmos;          // x = Rayleigh スケールハイト[m], y = Mie スケールハイト[m], z = Mie g, w = 散乱強度
    vec4  atmos2;         // x = Mie 密度倍率, y = 星の強度 (Profile curve), z = 天の川の強度 (curve), w = 露出
    vec4  resolution;     // xy = 画面サイズ, zw = 1/xy
    vec4  misc;           // x = sRGB 変換が必要なら 1, y = dt, z = シーン番号
    vec4  p[8];           // シーン固有パラメータ (HUD で編集)
    ivec4 flags;          // x = シーン固有トグル (bit)
    vec4  objs[16];       // シーン固有オブジェクト (雲の位置, 動く箱 など)
    vec4  profile_keys[48]; // Profile 可視化用: 4 トラック x 12 キー (rgb = 値, a = 時刻 0..1)
    ivec4 profile_counts;   // 各トラックのキー数
} F;

bool flag(int bit) { return (F.flags.x & (1 << bit)) != 0; }

// -----------------------------------------------------------------------------
// カメラレイ: スクリーン UV (0..1) → ワールド方向
// -----------------------------------------------------------------------------
vec3 camera_ray(vec2 uv) {
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 a = F.inv_view_proj * vec4(ndc, 0.0, 1.0);
    vec4 b = F.inv_view_proj * vec4(ndc, 1.0, 1.0);
    a.xyz /= a.w;
    b.xyz /= b.w;
    return normalize(b.xyz - a.xyz);
}

// ワールド座標 → Vulkan の深度 (0..1)。フルスクリーンのレイマーチ結果を
// 深度バッファへ書き込み、通常のメッシュ描画 (草) と前後関係を合わせるのに使う。
float world_to_depth(vec3 p) {
    vec4 c = F.view_proj * vec4(p, 1.0);
    return clamp(c.z / c.w, 0.0, 1.0);
}

// -----------------------------------------------------------------------------
// ハッシュ・ノイズ
// -----------------------------------------------------------------------------
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
float hash13(vec3 p3) {
    p3 = fract(p3 * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return fract((p3.x + p3.y) * p3.z);
}
vec2 hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}
vec3 hash33(vec3 p3) {
    p3 = fract(p3 * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yxz + 33.33);
    return fract((p3.xxy + p3.yxx) * p3.zyx);
}

float value_noise2(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1, 0)), u.x),
               mix(hash12(i + vec2(0, 1)), hash12(i + vec2(1, 1)), u.x), u.y);
}
float value_noise3(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    float a = hash13(i + vec3(0, 0, 0)), b = hash13(i + vec3(1, 0, 0));
    float c = hash13(i + vec3(0, 1, 0)), d = hash13(i + vec3(1, 1, 0));
    float e = hash13(i + vec3(0, 0, 1)), g = hash13(i + vec3(1, 0, 1));
    float h = hash13(i + vec3(0, 1, 1)), k = hash13(i + vec3(1, 1, 1));
    return mix(mix(mix(a, b, u.x), mix(c, d, u.x), u.y),
               mix(mix(e, g, u.x), mix(h, k, u.x), u.y), u.z);
}
float fbm2(vec2 p, int oct) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < oct; ++i) { s += a * value_noise2(p); p = p * 2.03 + 17.1; a *= 0.5; }
    return s;
}
float fbm3(vec3 p, int oct) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < oct; ++i) { s += a * value_noise3(p); p = p * 2.02 + 11.3; a *= 0.5; }
    return s;
}

// -----------------------------------------------------------------------------
// 出力: 露出 → ACES 近似トーンマップ → (必要なら) ガンマ
// -----------------------------------------------------------------------------
vec3 aces_fitted(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
vec4 finalize(vec3 hdr) {
    vec3 c = aces_fitted(hdr * F.atmos2.w);
    if (F.misc.x > 0.5) c = pow(c, vec3(1.0 / 2.2));
    return vec4(c, 1.0);
}

float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// -----------------------------------------------------------------------------
// SDF 基本形状
// -----------------------------------------------------------------------------
float sd_box(vec3 p, vec3 b) {
    vec3 q = abs(p) - b;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}
float sd_round_box(vec3 p, vec3 b, float r) { return sd_box(p, b - r) - r; }
float sd_sphere(vec3 p, float r) { return length(p) - r; }
float sd_capsule(vec3 p, vec3 a, vec3 b, float r) {
    vec3 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h) - r;
}
float sd_cyl_y(vec3 p, float r, float h) {
    vec2 d = abs(vec2(length(p.xz), p.y)) - vec2(r, h);
    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
}
float smin(float a, float b, float k) {
    float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return mix(b, a, h) - k * h * (1.0 - h);
}

#endif
