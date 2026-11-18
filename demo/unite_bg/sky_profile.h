#pragma once
// =============================================================================
// SkyTime と Profile
//   SkyTime : 1 日を 0..1 で持つ 1 つの値 (0.25 = 6:00, 0.5 = 12:00, 0.75 = 18:00)
//   Profile : 「1 日」を表すグラデーション / カーブの集まり。
//             各トラックは「時間で変化」と「固定値」を切り替えられる。
// =============================================================================

#include "demo_math.h"
#include <vector>

namespace ubg {

struct ColorKey { float time01; Vec3 color; };
struct CurveKey { float time01; float value; };

/// HDR/LDR Gradient: 左 (0:00) → 右 (24:00)。キー間は線形補間 (Blend モード)
struct GradientTrack {
    const char*           name = "";
    std::vector<ColorKey> keys;        // time01 昇順
    bool                  use_fixed = false;
    Vec3                  fixed_value{1, 1, 1};
    Vec3 evaluate(float t01) const;
};

/// Curve: 0..1 を 0..24 時として値を持つ。キー間は smoothstep で滑らかに
struct CurveTrack {
    const char*           name = "";
    std::vector<CurveKey> keys;
    bool                  use_fixed = false;
    float                 fixed_value = 1.0f;
    float evaluate(float t01) const;
};

/// 全シーンで共有する大気散乱の設定 (Profile の Atmosphere Settings 相当)
struct AtmosphereSettings {
    float wavelength[3]       = {680.0f, 550.0f, 450.0f};  // nm
    float molecular_density   = 1.0f;     // レイリー散乱係数の倍率
    float rayleigh_height     = 7994.0f;  // レイリーのスケールハイト [m]
    float mie_height          = 1200.0f;  // ミーのスケールハイト [m]
    float mie_density         = 1.0f;     // ミー散乱係数の倍率
    float mie_g               = 0.76f;    // Mie Directionality Factor (前方散乱の強さ)
    float scattering_intensity = 20.0f;   // 散乱の明るさ
};

struct SkyProfile {
    AtmosphereSettings atmosphere;
    GradientTrack sunlight_color;     // 平行光の色
    CurveTrack    sunlight_intensity;
    GradientTrack rayleigh_color;     // レイリー計算結果に掛ける色
    GradientTrack mie_color;          // ミー計算結果に掛ける色
    GradientTrack moonlight_color;
    CurveTrack    moonlight_intensity;
    CurveTrack    star_intensity;
    CurveTrack    galaxy_intensity;

    static SkyProfile make_default();
};

/// SkyTime から太陽・月の方向
Vec3 sun_direction(float sky_time01);
Vec3 moon_direction(float sky_time01);

/// Timeline のクリップブレンド: 時刻 x weight の和 = 時刻の補間
/// (0:00 をまたぐと破綻する点に注意。デモの Scene 9 で確認できる)
float blend_clip_times(float time_a01, float weight_a, float time_b01, float weight_b);

inline float hours(float h, float m = 0.0f) { return (h + m / 60.0f) / 24.0f; }

} // namespace ubg
