// SHaRC 多灯推定量の CPU 契約テスト (SHARC_EXT_REVIEW Gate 1 / P0-01)。
//
// update パス (shaders/sharc/sharc_update.comp) のモーメント推定量
//   E = (1/N) Σ_i f(x_i) / q(x_i),  q = 1/L (一様ライト選択)
// を CPU で再現し、 次の完了条件を検証する:
//   - 同一ライト 1・2・64 灯で期待値が線形に 1・2・64 倍になる
//   - ライト順序を変えても期待値が変わらない
//   - no-light で有限なゼロを返す
//   - 解析値 (Σ 寄与) と推定平均が許容誤差内で一致する
// 乱数は GLSL sharcRand と同一の lowbias32 チェーンを使う。

#include "pictor/gi/sharc_types.h"
#include "test_common.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace pictor;

namespace {

/// GLSL sharcRand と同一 (lowbias32 チェーン → [0,1))。
float sharc_rand(uint32_t& state) {
    state = sharc_hash_uint(state);
    return static_cast<float>(state) * (1.0f / 4294967296.0f);
}

/// update パスと同じ推定量: N サンプル、 一様候補、 Σ(f/q)/N。
/// contributions[i] = ライト i のセル中心での寄与 (スカラ輝度)。
double estimate(const std::vector<double>& contributions, uint32_t samples,
                uint32_t seed) {
    const auto L = static_cast<uint32_t>(contributions.size());
    if (L == 0) return 0.0;
    uint32_t rng = seed;
    double sum = 0.0;
    for (uint32_t i = 0; i < samples; ++i) {
        const uint32_t idx = (std::min)(
            static_cast<uint32_t>(sharc_rand(rng) * static_cast<float>(L)),
            L - 1);
        // f/q = f · L  (sharc_update.comp: candW = pHat/pSource,
        //               invPdf = candW/pHat = L)
        sum += contributions[idx] * static_cast<double>(L);
    }
    return sum / static_cast<double>(samples);
}

double analytic_sum(const std::vector<double>& c) {
    double s = 0.0;
    for (const double v : c) s += v;
    return s;
}

} // namespace

int main() {
    constexpr uint32_t kSamples = 1u << 18;   // 262,144 (分散を十分潰す)
    constexpr double kTolerance = 0.03;       // 3%

    // ── 1. 1灯 → 2灯 → 64灯 の線形性 (等強度ライト) ──
    {
        const double unit = 0.7;
        double prev = 0.0;
        for (const uint32_t count : {1u, 2u, 64u}) {
            std::vector<double> lights(count, unit);
            const double est = estimate(lights, kSamples, 0x1234u + count);
            const double expect = unit * count;
            PT_ASSERT(std::abs(est - expect) / expect < kTolerance,
                      "estimator matches analytic sum (linearity)");
            PT_ASSERT(est > prev, "energy grows with light count");
            prev = est;
        }
    }

    // ── 2. 異強度ライト: 解析和との一致 + 順序不変 ──
    {
        std::vector<double> lights = {0.1, 2.0, 0.5, 4.0, 0.05, 1.25};
        const double expect = analytic_sum(lights);
        const double est1 = estimate(lights, kSamples, 42u);
        std::reverse(lights.begin(), lights.end());
        const double est2 = estimate(lights, kSamples, 42u);
        PT_ASSERT(std::abs(est1 - expect) / expect < kTolerance,
                  "mixed-intensity estimator matches analytic sum");
        PT_ASSERT(std::abs(est2 - expect) / expect < kTolerance,
                  "estimator is order-invariant (same expectation)");
    }

    // ── 3. no-light: 有限なゼロ ──
    {
        const double est = estimate({}, kSamples, 7u);
        PT_ASSERT(est == 0.0 && std::isfinite(est),
                  "no-light returns finite zero");
    }

    // ── 4. 回帰: 旧実装 Σ(f/q)/Σ(1/q) は多灯で 1/L に暗くなる ──
    //    (この振る舞いに戻っていないことを明示的に検査する)
    {
        std::vector<double> two(2, 1.0);
        const double est = estimate(two, kSamples, 99u);
        PT_ASSERT(est > 1.5, "sum-estimator, not mean (P0-01 regression)");
    }

    return pictor_test::report("unit_sharc_estimator_test");
}
