/// GI probe field / scene proxy / SH ユーティリティの headless 検証。
///
/// `spec/feature/gi-bake-realtime-design.md` phase 1。 GPU を要さない
/// CPU 経路 (遮蔽クエリ / SH 射影 / probe grid 構築 / relight / 補間) を見る。

#include "pictor/gi/gi_probe_field.h"
#include "pictor/gi/gi_scene_proxy.h"
#include "pictor/gi/gi_sh.h"
#include "test_common.h"

#include <cmath>
#include <cstring>
#include <vector>

using namespace pictor;
using namespace pictor_test;

namespace {

AABB box(float cx, float cy, float cz, float rx, float ry, float rz) {
    AABB a;
    a.min = {cx - rx, cy - ry, cz - rz};
    a.max = {cx + rx, cy + ry, cz + rz};
    return a;
}

bool feq(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) < eps;
}

} // namespace

int main() {
    // 1. SH — 等方放射の射影 / 復元。 全方向から一定放射 L を射影すると、
    //    任意法線の irradiance が L に近づく (モンテカルロ + cosine 畳み込み)。
    {
        float sh[kSHFloatCount] = {};
        const uint32_t N = 256;
        const float3 L = {2.0f, 1.0f, 0.5f};
        const float w = 4.0f * 3.14159265f / static_cast<float>(N);
        for (uint32_t i = 0; i < N; ++i) {
            sh_add_radiance(sh, fibonacci_sphere_dir(i, N), L, w);
        }
        const float3 up_irr = sh_eval_irradiance(sh, {0.0f, 1.0f, 0.0f});
        const float3 dn_irr = sh_eval_irradiance(sh, {0.0f, -1.0f, 0.0f});
        // 等方 L の irradiance は (π L) / π = L に戻る規約。
        PT_ASSERT(feq(up_irr.x, L.x, 0.1f), "isotropic radiance round-trips");
        PT_ASSERT(feq(up_irr.x, dn_irr.x, 1e-2f), "isotropic: up == down (R)");
        PT_ASSERT(feq(up_irr.y, dn_irr.y, 1e-2f), "isotropic: up == down (G)");
        PT_ASSERT(up_irr.x > up_irr.y && up_irr.y > up_irr.z,
                  "channel ordering follows input radiance");
    }

    // 2. SH — 方向性。 上方からの delta 射影は上向き法線で明るく、
    //    下向き法線でほぼ 0。
    {
        float sh[kSHFloatCount] = {};
        sh_add_radiance(sh, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 1.0f);
        const float3 up = sh_eval_irradiance(sh, {0.0f, 1.0f, 0.0f});
        const float3 dn = sh_eval_irradiance(sh, {0.0f, -1.0f, 0.0f});
        PT_ASSERT(up.x > 0.1f, "delta from above lights up-facing normal");
        PT_ASSERT(dn.x < up.x * 0.2f, "down-facing normal stays dark");
    }

    // 3. fibonacci sphere — 単位長 + 決定性。
    {
        const float3 a = fibonacci_sphere_dir(7, 64);
        const float3 b = fibonacci_sphere_dir(7, 64);
        PT_ASSERT(feq(a.x, b.x) && feq(a.y, b.y) && feq(a.z, b.z),
                  "fibonacci dir deterministic");
        const float len = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
        PT_ASSERT(feq(len, 1.0f, 1e-3f), "fibonacci dir unit length");
    }

    // 4. GISceneProxy — 遮蔽 / 最近ヒット / self-ignore。
    {
        std::vector<AABB> boxes = {
            box(0.0f, 5.0f, 0.0f, 10.0f, 0.5f, 10.0f),   // 天井 (y=4.5..5.5)
            box(3.0f, 0.0f, 0.0f, 0.5f, 0.5f, 0.5f),      // 隣接キューブ
        };
        std::vector<ObjectId> ids = {100, 200};
        GISceneProxy proxy;
        proxy.build(boxes.data(), ids.data(), 2);

        // 原点から上 → 天井に遮られる。
        PT_ASSERT(proxy.occluded({0, 0, 0}, {0, 1, 0}, 100.0f),
                  "ray up hits ceiling");
        // 天井を ignore すると抜ける。
        PT_ASSERT(!proxy.occluded({0, 0, 0}, {0, 1, 0}, 100.0f, 100),
                  "ignore skips ceiling");
        // 下向きは何もない。
        PT_ASSERT(!proxy.occluded({0, 0, 0}, {0, -1, 0}, 100.0f),
                  "ray down misses");
        // max_dist 内に届かなければ遮蔽なし。
        PT_ASSERT(!proxy.occluded({0, 0, 0}, {0, 1, 0}, 2.0f),
                  "short ray stops before ceiling");

        // closest_hit: +x 方向はキューブ面 (x=2.5) が先。
        const auto hit = proxy.closest_hit({0, 0, 0}, {1, 0, 0}, 100.0f);
        PT_ASSERT(hit.hit, "closest hit found");
        PT_ASSERT(feq(hit.distance, 2.5f, 1e-3f), "closest hit distance = 2.5");
        PT_ASSERT_OP(proxy.object_id(hit.index), ==, ObjectId{200},
                     "closest hit id");
    }

    // 5. sample_probe_grid — trilinear 補間。 2 probe (x 軸) の中点は平均。
    {
        GIProbeConfig cfg;
        cfg.grid_origin  = {0, 0, 0};
        cfg.grid_spacing = {2, 2, 2};
        cfg.grid_x = 2; cfg.grid_y = 1; cfg.grid_z = 1;

        std::vector<float> sh(2 * kSHFloatCount, 0.0f);
        sh[0] = 1.0f;                     // probe0 の係数 0 (R)
        sh[kSHFloatCount] = 3.0f;         // probe1 の係数 0 (R)

        float out[kSHFloatCount];
        sample_probe_grid(cfg, sh.data(), {1.0f, 0.0f, 0.0f}, out);
        PT_ASSERT(feq(out[0], 2.0f), "midpoint = average of probes");

        sample_probe_grid(cfg, sh.data(), {0.0f, 0.0f, 0.0f}, out);
        PT_ASSERT(feq(out[0], 1.0f), "at probe0 = probe0 value");

        // grid 外はクランプ。
        sample_probe_grid(cfg, sh.data(), {100.0f, 50.0f, -9.0f}, out);
        PT_ASSERT(feq(out[0], 3.0f), "outside clamps to edge probe");
    }

    // 6. GIProbeField — 床上の probe grid。 空が見える probe は非ゼロ SH、
    //    relight で太陽を消すと間接光 (バウンス) が消える。
    {
        // 床 (y = -0.5 .. 0)、 10x10。
        std::vector<AABB> boxes = {box(0, -0.25f, 0, 10.0f, 0.25f, 10.0f)};
        std::vector<ObjectId> ids = {1};
        GISceneProxy proxy;
        proxy.build(boxes.data(), ids.data(), 1);

        GIProbeConfig cfg;
        cfg.grid_origin  = {-2, 1, -2};
        cfg.grid_spacing = {2, 2, 2};
        cfg.grid_x = 3; cfg.grid_y = 2; cfg.grid_z = 3;

        DirectionalLight sun;
        sun.direction = {0.0f, -1.0f, 0.0f};
        sun.intensity = 2.0f;
        sun.color     = {1.0f, 0.9f, 0.8f};

        GIProbeField field;
        GIProbeField::BuildParams params;
        params.rays_per_probe = 64;
        field.build(cfg, proxy, sun, {}, params);

        PT_ASSERT(field.built(), "field built");
        PT_ASSERT_OP(field.probe_count(), ==, uint32_t{18}, "3*2*3 probes");

        // 床上 1m の点の irradiance: 上向き法線 = 空、 下向き法線 = 床バウンス。
        const float3 p = {0.0f, 1.0f, 0.0f};
        const float3 up_irr = field.sample_irradiance(p, {0, 1, 0});
        const float3 dn_irr = field.sample_irradiance(p, {0, -1, 0});
        PT_ASSERT(up_irr.x > 0.0f, "sky contributes to up-facing normal");
        PT_ASSERT(dn_irr.x > 0.0f, "floor bounce lights down-facing normal");

        // relight: 太陽 0 → バウンス消滅、 空は残る (決定的に比較)。
        DirectionalLight dark = sun;
        dark.intensity = 0.0f;
        field.relight(dark, {});
        const float3 dn_dark = field.sample_irradiance(p, {0, -1, 0});
        PT_ASSERT(dn_dark.x < dn_irr.x,
                  "bounce disappears when sun goes out");
        const float3 up_dark = field.sample_irradiance(p, {0, 1, 0});
        PT_ASSERT(up_dark.x > 0.0f, "sky remains after relight");

        // relight で元に戻せる (キャッシュ再利用の決定性)。
        field.relight(sun, {});
        const float3 dn_again = field.sample_irradiance(p, {0, -1, 0});
        PT_ASSERT(feq(dn_again.x, dn_irr.x, 1e-4f),
                  "relight is deterministic (cache reuse)");
    }

    // 7. include_direct — 既定では太陽 delta が SH に入らない (二重計上防止)。
    {
        GISceneProxy proxy;   // 空シーン
        proxy.build(nullptr, nullptr, 0);

        GIProbeConfig cfg;
        cfg.grid_x = 1; cfg.grid_y = 1; cfg.grid_z = 1;

        DirectionalLight sun;
        sun.direction = {0.0f, -1.0f, 0.0f};
        sun.intensity = 5.0f;

        GIProbeField::BuildParams p1;   // include_direct = false (既定)
        p1.sky_intensity = 0.0f;        // 空も消して直接光だけを観測
        GIProbeField field;
        field.build(cfg, proxy, sun, {}, p1);
        const float3 no_direct =
            field.sample_irradiance({0, 0, 0}, {0, 1, 0});
        PT_ASSERT(feq(no_direct.x, 0.0f, 1e-4f),
                  "default: sun delta excluded from probes");

        GIProbeField::BuildParams p2 = p1;
        p2.include_direct = true;
        field.build(cfg, proxy, sun, {}, p2);
        const float3 with_direct =
            field.sample_irradiance({0, 0, 0}, {0, 1, 0});
        PT_ASSERT(with_direct.x > 0.1f,
                  "include_direct folds sun into probes");
    }

    return report("unit_gi_probe_field_test");
}
