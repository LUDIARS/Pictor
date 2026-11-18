#include "sky_profile.h"

namespace ubg {

Vec3 GradientTrack::evaluate(float t) const {
    if (use_fixed || keys.empty()) return use_fixed ? fixed_value : Vec3{0, 0, 0};
    if (t <= keys.front().time01) return keys.front().color;
    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        const ColorKey& a = keys[i];
        const ColorKey& b = keys[i + 1];
        if (t <= b.time01) {
            float u = (t - a.time01) / std::fmax(b.time01 - a.time01, 1e-5f);
            return a.color + (b.color - a.color) * u;
        }
    }
    return keys.back().color;
}

float CurveTrack::evaluate(float t) const {
    if (use_fixed || keys.empty()) return use_fixed ? fixed_value : 0.0f;
    if (t <= keys.front().time01) return keys.front().value;
    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        const CurveKey& a = keys[i];
        const CurveKey& b = keys[i + 1];
        if (t <= b.time01) {
            float u = smoothstepf(0.0f, 1.0f, (t - a.time01) / std::fmax(b.time01 - a.time01, 1e-5f));
            return lerpf(a.value, b.value, u);
        }
    }
    return keys.back().value;
}

SkyProfile SkyProfile::make_default() {
    SkyProfile p;

    // 平行光: 色が速く変わる朝と夕方にキーを集中させる
    p.sunlight_color.name = "Sunlight Color";
    p.sunlight_color.keys = {
        {hours(0),      {0.00f, 0.00f, 0.00f}},
        {hours(5, 37),  {0.15f, 0.03f, 0.03f}},
        {hours(6, 0),   {1.00f, 0.30f, 0.08f}},
        {hours(6, 17),  {1.00f, 0.58f, 0.20f}},
        {hours(8, 24),  {1.00f, 0.93f, 0.82f}},
        {hours(12, 0),  {1.00f, 1.00f, 0.98f}},
        {hours(15, 36), {1.00f, 0.96f, 0.88f}},
        {hours(17, 42), {1.00f, 0.62f, 0.22f}},
        {hours(17, 59), {1.00f, 0.32f, 0.10f}},
        {hours(18, 45), {0.08f, 0.02f, 0.02f}},
        {hours(24),     {0.00f, 0.00f, 0.00f}},
    };
    p.sunlight_intensity.name = "Sunlight Intensity";
    p.sunlight_intensity.keys = {
        {hours(0), 0.0f}, {hours(5, 40), 0.0f}, {hours(6, 20), 1.0f},
        {hours(17, 40), 1.0f}, {hours(18, 40), 0.0f}, {hours(24), 0.0f},
    };

    p.rayleigh_color.name = "Rayleigh Color";
    p.rayleigh_color.keys = {
        {hours(0), {0.55f, 0.65f, 1.0f}}, {hours(5, 30), {0.75f, 0.80f, 1.0f}},
        {hours(7, 0), {1.0f, 1.0f, 1.0f}}, {hours(17, 0), {1.0f, 1.0f, 1.0f}},
        {hours(19, 0), {0.75f, 0.75f, 1.0f}}, {hours(24), {0.55f, 0.65f, 1.0f}},
    };
    p.mie_color.name = "Mie Color";
    p.mie_color.keys = {
        {hours(0), {1.0f, 1.0f, 1.0f}}, {hours(5, 40), {1.0f, 0.70f, 0.45f}},
        {hours(7, 0), {1.0f, 1.0f, 1.0f}}, {hours(16, 40), {1.0f, 1.0f, 1.0f}},
        {hours(18, 10), {1.0f, 0.55f, 0.30f}}, {hours(19, 30), {1.0f, 1.0f, 1.0f}},
        {hours(24), {1.0f, 1.0f, 1.0f}},
    };

    p.moonlight_color.name = "Moonlight Color";
    p.moonlight_color.keys = {{hours(0), {0.55f, 0.65f, 1.0f}}, {hours(24), {0.55f, 0.65f, 1.0f}}};
    p.moonlight_intensity.name = "Moonlight Intensity";
    p.moonlight_intensity.keys = {
        {hours(0), 0.06f}, {hours(5, 20), 0.06f}, {hours(6, 20), 0.0f},
        {hours(17, 50), 0.0f}, {hours(19, 0), 0.06f}, {hours(24), 0.06f},
    };

    // 星の強度: 夜から徐々に見えるように (スライドのカーブを模したもの)
    p.star_intensity.name = "Star Intensity";
    p.star_intensity.keys = {
        {hours(0), 2.0f}, {hours(2, 28), 1.95f}, {hours(3, 59), 0.90f}, {hours(5, 56), 0.05f},
        {hours(18, 0), 0.05f}, {hours(20, 30), 0.30f}, {hours(22, 37), 1.93f}, {hours(24), 2.0f},
    };
    // 天の川: 深夜だけ
    p.galaxy_intensity.name = "Galaxy Intensity";
    p.galaxy_intensity.keys = {
        {hours(0), 1.0f}, {hours(1, 30), 1.0f}, {hours(2, 0), 0.0f},
        {hours(23, 31), 0.0f}, {hours(24), 1.0f},
    };
    return p;
}

Vec3 sun_direction(float t) {
    // 6:00 に東 (+X) から昇り、12:00 に南寄りの高い位置、18:00 に西 (-X) へ沈む
    float h = (t - 0.25f) * 2.0f * kPi;
    return normalize(Vec3{std::cos(h), std::sin(h) * 0.92f, 0.35f});
}

Vec3 moon_direction(float t) {
    float h = (t - 0.25f) * 2.0f * kPi + kPi;
    return normalize(Vec3{std::cos(h), std::sin(h) * 0.85f, 0.20f});
}

float blend_clip_times(float a, float wa, float b, float wb) {
    return a * wa + b * wb;
}

} // namespace ubg
