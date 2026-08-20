/// GIBakeSystem — CPU ベイク実演算の headless 検証。
///
/// `spec/feature/gi-bake-realtime-design.md` phase 1。 static pool に
/// 既知配置 (開けた場所 / 屋根の下 / 壁ぎわ) のオブジェクトを置き、
/// AO / shadow / lightmap / irradiance が幾何を反映することを見る。
/// save/load round-trip とキャンセルも確認する。

#include "pictor/gi/gi_bake.h"
#include "pictor/gi/gi_lighting_system.h"
#include "pictor/memory/memory_subsystem.h"
#include "pictor/scene/scene_registry.h"
#include "test_common.h"

#include <cmath>
#include <cstdio>

using namespace pictor;
using namespace pictor_test;

namespace {

AABB box(float cx, float cy, float cz, float rx, float ry, float rz) {
    AABB a;
    a.min = {cx - rx, cy - ry, cz - rz};
    a.max = {cx + rx, cy + ry, cz + rz};
    return a;
}

ObjectId add_static(SceneRegistry& reg, const AABB& bounds) {
    ObjectDescriptor desc;
    desc.flags  = ObjectFlags::STATIC;
    desc.bounds = bounds;
    float4x4 t = float4x4::identity();
    const float3 c = bounds.center();
    t.set_translation(c.x, c.y, c.z);
    desc.transform = t;
    return reg.register_object(desc);
}

bool feq(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) < eps;
}

float4x4 perspective(float near_z, float far_z) {
    float4x4 p{};
    p.m[0][0] = 1.0f;
    p.m[1][1] = -1.0f;
    p.m[2][2] = far_z / (near_z - far_z);
    p.m[2][3] = -1.0f;
    p.m[3][2] = (near_z * far_z) / (near_z - far_z);
    return p;
}

bool matrix_is_finite(const float4x4& m) {
    for (const auto& row : m.m) {
        for (float value : row) {
            if (!std::isfinite(value)) return false;
        }
    }
    return true;
}

} // namespace

int main() {
    MemoryConfig mem_cfg;
    MemorySubsystem memory(mem_cfg);
    SceneRegistry   registry(memory);
    GpuMemoryAllocator gpu_alloc;
    GPUBufferManager   buffers(gpu_alloc);
    GILightingSystem   gi(buffers, registry);

    // ── シーン ────────────────────────────────────────────────────────
    // index 0: 開けた場所に浮かぶキューブ (遮蔽なし)
    // index 1: 屋根スラブ (y=4..4.5、 下のキューブを覆う)
    // index 2: 屋根の真下のキューブ
    // index 3/4: 壁 + 壁ぎわのキューブ (AO 比較用、 遠方に隔離)
    const ObjectId open_id  = add_static(registry, box(0, 10, 0, 0.5f, 0.5f, 0.5f));
    const ObjectId roof_id  = add_static(registry, box(50, 4.25f, 0, 3.0f, 0.25f, 3.0f));
    const ObjectId under_id = add_static(registry, box(50, 1, 0, 0.5f, 0.5f, 0.5f));
    const ObjectId wall_id  = add_static(registry, box(100.8f, 10, 0, 0.25f, 2.0f, 2.0f));
    const ObjectId near_id  = add_static(registry, box(100, 10, 0, 0.5f, 0.5f, 0.5f));
    (void)roof_id; (void)wall_id;

    const uint32_t static_count = registry.static_pool().count();
    PT_ASSERT_OP(static_count, ==, uint32_t{5}, "5 static objects registered");

    // CSM split distances may intentionally extend beyond the camera far
    // plane. They must remain distinct under inverse projection; clamping both
    // ends to NDC z=1 collapses the last cascade and produces a non-finite
    // orthographic matrix.
    GIConfig runtime_cfg;
    runtime_cfg.shadow.cascade_count = 4;
    runtime_cfg.shadow.resolution = 16;
    runtime_cfg.shadow.max_shadow_dist = 200.0f;
    runtime_cfg.ssao_enabled = false;
    runtime_cfg.gi_probes_enabled = false;
    gi.set_config(runtime_cfg);
    gi.initialize(static_count, 16, 16);
    gi.execute(float4x4::identity(), perspective(0.1f, 50.0f));
    for (uint32_t cascade = 0; cascade < runtime_cfg.shadow.cascade_count; ++cascade) {
        PT_ASSERT(matrix_is_finite(gi.cascade_view_proj(cascade)),
                  "CSM matrix remains finite beyond camera far plane");
    }

    GIBakeSystem baker(buffers, registry, gi);

    DirectionalLight sun;
    sun.direction = {0.0f, -1.0f, 0.0f};
    sun.intensity = 1.0f;
    sun.color     = {1.0f, 1.0f, 1.0f};
    baker.set_directional_light(sun);

    GIBakeConfig cfg;
    cfg.targets = BakeTarget::ALL;
    cfg.ao.sample_count = 64;
    cfg.ao.radius       = 1.5f;
    // probe grid をシーン全域 (x 0..100) をまたぐ粗い格子に。
    cfg.probes.grid_origin  = {-2.0f, 0.0f, -4.0f};
    cfg.probes.grid_spacing = {26.0f, 4.0f, 4.0f};
    cfg.probes.grid_x = 5; cfg.probes.grid_y = 3; cfg.probes.grid_z = 3;
    cfg.lightmap.samples_per_texel = 64;
    baker.set_config(cfg);

    // ── bake (progress 監視付き) ─────────────────────────────────────
    int progress_calls = 0;
    GIBakeResult result = baker.bake(
        [&progress_calls](float, const char*) {
            ++progress_calls;
            return true;
        });

    PT_ASSERT(result.valid, "bake result valid");
    PT_ASSERT(baker.is_baked() && baker.is_valid(), "baker state after bake");
    PT_ASSERT(progress_calls > 0, "progress callback invoked");
    PT_ASSERT_OP(result.object_ids.size(), ==, size_t{static_count},
                 "ids for every static object");
    PT_ASSERT_OP(result.ao.size(),         ==, size_t{static_count}, "ao baked");
    PT_ASSERT_OP(result.shadows.size(),    ==, size_t{static_count}, "shadows baked");
    PT_ASSERT_OP(result.irradiance.size(), ==, size_t{static_count}, "irradiance baked");
    PT_ASSERT_OP(result.lightmaps.size(),  ==, size_t{static_count}, "lightmaps baked");

    // pool index を ObjectId から引く。
    auto index_of = [&result](ObjectId id) -> int {
        for (size_t i = 0; i < result.object_ids.size(); ++i)
            if (result.object_ids[i] == id) return static_cast<int>(i);
        return -1;
    };
    const int open_i  = index_of(open_id);
    const int under_i = index_of(under_id);
    const int near_i  = index_of(near_id);
    PT_ASSERT(open_i >= 0 && under_i >= 0 && near_i >= 0, "ids resolvable");

    // ── AO — 遮蔽なし = 1.0、 壁ぎわ / 屋根下 < 1.0 ─────────────────────
    PT_ASSERT(feq(result.ao[open_i].occlusion, 1.0f, 1e-3f),
              "open object: no occlusion");
    PT_ASSERT(result.ao[near_i].occlusion < 0.999f,
              "wall-adjacent object: occluded");
    PT_ASSERT(result.ao[near_i].occlusion < result.ao[open_i].occlusion,
              "AO reflects geometry");

    // ── shadow — 開けた場所は全可視 (1.0)、 屋根下は遮蔽 ──────────────
    PT_ASSERT(feq(result.shadows[open_i].depths[0], 1.0f, 1e-3f),
              "open object: fully sun-lit");
    PT_ASSERT(result.shadows[under_i].depths[0] < 0.5f,
              "roofed object: sun blocked");
    PT_ASSERT_OP(result.shadows[open_i].cascade_flags, !=, uint32_t{0},
                 "cascade flags set");

    // ── lightmap — 開けた場所は direct > 0、 屋根下は direct ≈ 0 ─────
    PT_ASSERT(result.lightmaps[open_i].direct_r > 0.5f,
              "open object: direct sun");
    PT_ASSERT(result.lightmaps[under_i].direct_r < 0.05f,
              "roofed object: direct blocked");
    PT_ASSERT(result.lightmaps[open_i].indirect_r >= 0.0f,
              "indirect non-negative");

    // ── irradiance — 空の寄与でどこかは非ゼロ ─────────────────────────
    bool any_irradiance = false;
    for (const auto& ir : result.irradiance) {
        for (float v : ir.sh) {
            if (std::fabs(v) > 1e-6f) { any_irradiance = true; break; }
        }
    }
    PT_ASSERT(any_irradiance, "probe irradiance produced");

    // ── 決定性 — 同条件の再ベイクは同値 ───────────────────────────────
    GIBakeResult again = baker.bake();
    PT_ASSERT(feq(again.ao[near_i].occlusion, result.ao[near_i].occlusion),
              "bake deterministic (ao)");
    PT_ASSERT(feq(again.lightmaps[open_i].direct_r,
                  result.lightmaps[open_i].direct_r),
              "bake deterministic (lightmap)");

    // ── save / load round-trip ────────────────────────────────────────
    const char* path = "gi_bake_test_roundtrip.bin";
    PT_ASSERT(baker.save(path, result), "save succeeds");
    GIBakeResult loaded = baker.load(path);
    PT_ASSERT(loaded.valid, "load succeeds");
    PT_ASSERT_OP(loaded.object_ids.size(), ==, result.object_ids.size(),
                 "round-trip: ids");
    PT_ASSERT(feq(loaded.ao[near_i].occlusion, result.ao[near_i].occlusion),
              "round-trip: ao");
    PT_ASSERT(feq(loaded.lightmaps[open_i].direct_r,
                  result.lightmaps[open_i].direct_r),
              "round-trip: lightmap");
    PT_ASSERT(feq(loaded.irradiance[open_i].sh[0],
                  result.irradiance[open_i].sh[0]),
              "round-trip: irradiance sh");
    std::remove(path);

    // ── キャンセル — 最初の progress で false → valid にならない ───────
    GIBakeResult cancelled = baker.bake(
        [](float, const char*) { return false; });
    PT_ASSERT(!cancelled.valid, "cancelled bake is not valid");

    // ── apply / invalidate の状態遷移 ─────────────────────────────────
    baker.apply(result);
    PT_ASSERT(baker.is_baked(), "apply keeps baked state");
    baker.invalidate();
    PT_ASSERT(baker.is_dirty() && !baker.is_valid(),
              "invalidate marks stale");

    return report("unit_gi_bake_test");
}
