/// Perf introspection unit test (headless, Vulkan 不要)。
/// spec/subsystem/perf_introspection.md §6 を裏取りする。

#include "pictor/profiler/perf_query_api.h"
#include "pictor/profiler/hardware_counters.h"
#include "pictor/scene/scene_registry.h"
#include "pictor/memory/memory_subsystem.h"
#include "pictor/batch/batch_builder.h"
#include "pictor/profiler/profiler.h"
#include "test_common.h"

#include <string>

// Win32 GDI (wingdi.h) の `TRANSPARENT` マクロ衝突を外す
// (ObjectFlags::TRANSPARENT が `::1` に化けるのを防ぐ)。
#ifdef TRANSPARENT
#undef TRANSPARENT
#endif

using namespace pictor;
using namespace pictor_test;

namespace {

ObjectDescriptor make_obj(MeshHandle mesh, uint32_t material, uint16_t flags) {
    ObjectDescriptor d;
    d.mesh        = mesh;
    d.material    = material;
    d.materialKey = material;
    d.shaderKey   = 0;
    d.flags       = flags;
    d.bounds      = AABB{ float3{0,0,0}, float3{1,1,1} };
    return d;
}

const StreamLayoutInfo* find_stream(const MemoryLayoutReport& r,
                                    PoolType pool, const std::string& name) {
    for (const auto& p : r.pools) {
        if (p.pool != pool) continue;
        for (const auto& s : p.streams) {
            if (s.name == name) return &s;
        }
    }
    return nullptr;
}

} // namespace

int main() {
    MemorySubsystem memory;
    SceneRegistry   scene(memory);
    BatchBuilder    batches(scene);
    Profiler        profiler;

    // 同一 mesh/material の 4 オブジェクト + 別 mesh 1 + 透過 2
    // (透過は順序制約で畳めない理由を出すため複数登録する)。
    for (int i = 0; i < 4; ++i) scene.register_object(make_obj(10, 5, ObjectFlags::DYNAMIC));
    scene.register_object(make_obj(20, 5, ObjectFlags::DYNAMIC));
    for (int i = 0; i < 2; ++i)
        scene.register_object(make_obj(30, 7, ObjectFlags::DYNAMIC | ObjectFlags::TRANSPARENT));

    batches.build(memory.frame_allocator());

    PerfQueryAPI perf(scene, memory, batches, profiler);

    // ---- A. メモリレイアウト ----
    {
        const MemoryLayoutReport r = perf.memory_layout_report();
        PT_ASSERT_OP(r.cache_line_size, ==, 64u, "cache line 64");

        const StreamLayoutInfo* bounds = find_stream(r, PoolType::DYNAMIC, "bounds");
        PT_ASSERT(bounds != nullptr, "bounds stream present");
        if (bounds) {
            // PoolAllocator は 64B 整列を保証する (裏取り)。
            PT_ASSERT(bounds->base_cache_aligned, "bounds base 64B aligned");
            PT_ASSERT_OP(bounds->element_size, ==, 24u, "AABB == 24B");
            // 24B は 64 の非約数 → 要素がキャッシュライン跨ぎ。
            PT_ASSERT(bounds->element_straddles_line, "AABB straddles cache line");
        }

        const StreamLayoutInfo* xf = find_stream(r, PoolType::DYNAMIC, "transforms");
        PT_ASSERT(xf != nullptr, "transforms stream present");
        if (xf) {
            PT_ASSERT_OP(xf->element_size, ==, 64u, "float4x4 == 64B");
            PT_ASSERT(!xf->element_straddles_line, "float4x4 does not straddle");
            PT_ASSERT(xf->base_cache_aligned, "transforms base 64B aligned");
            // 64*N は 64 の倍数 → 100% 利用。
            PT_ASSERT_OP((long long)(xf->line_utilization_pct + 0.5), ==, 100LL,
                         "transforms line utilization 100%");
        }

        // 断片化は total >= reserved >= used。
        PT_ASSERT_OP(r.soa_reserved_bytes, >=, r.soa_used_bytes, "reserved >= used");
        PT_ASSERT_OP(r.pool_allocator_total_bytes, >=, r.soa_reserved_bytes,
                     "allocator total >= reserved");

        // DoD 不変条件が全て ok。
        bool all_ok = !r.invariants.empty();
        for (const auto& inv : r.invariants) {
            if (!inv.ok) {
                std::fprintf(stderr, "  invariant FAIL: %s actual=%zu expected=%zu\n",
                             inv.name.c_str(), inv.actual, inv.expected);
                all_ok = false;
            }
        }
        PT_ASSERT(all_ok, "all DoD invariants ok");
    }

    // ---- バッチ適格性 ----
    {
        const BatchEligibilityReport r = perf.batch_eligibility_report();
        PT_ASSERT_OP(r.total_objects, ==, 7u, "7 objects total");
        // 期待グループ: (mesh10/mat5) x4, (mesh20/mat5) x1, (mesh30/mat7,透過) x2
        PT_ASSERT_OP((unsigned)r.groups.size(), ==, 3u, "3 batch groups");

        const BatchEligibilityGroup* g_main = nullptr;
        const BatchEligibilityGroup* g_trans = nullptr;
        for (const auto& g : r.groups) {
            if (g.mesh == 10) g_main = &g;
            if (g.mesh == 30) g_trans = &g;
        }
        PT_ASSERT(g_main != nullptr, "mesh10 group present");
        if (g_main) {
            PT_ASSERT_OP(g_main->object_count, ==, 4u, "mesh10 has 4 objects");
            PT_ASSERT(g_main->gpu_batchable, "mesh10 group is GPU-batchable");
            PT_ASSERT_OP(g_main->ideal_batches, ==, 1u, "mesh10 ideal = 1 batch");
        }
        PT_ASSERT(g_trans != nullptr, "transparent group present");
        if (g_trans) {
            PT_ASSERT(!g_trans->gpu_batchable, "transparent group not freely batchable");
            bool has_reason = false;
            for (auto rr : g_trans->reasons)
                if (rr == BatchSplitReason::Transparent) has_reason = true;
            PT_ASSERT(has_reason, "transparent group reason = Transparent");
        }
    }

    // ---- C. HW カウンタ (null provider, 理由付き) ----
    {
        const HwCounterSample s = perf.hw_counters();
        PT_ASSERT(!s.available, "no provider → unavailable");
        PT_ASSERT(!s.unavailable_reason.empty(), "unavailable reason non-empty");

        auto provider = create_hardware_counter_provider();
        PT_ASSERT(provider != nullptr, "factory returns a provider");
        // ビルドフラグ OFF の既定では利用不可だが理由は必ずある。
        if (provider && !provider->available()) {
            PT_ASSERT(provider->unavailable_reason()[0] != '\0',
                      "null provider reports a reason");
        }
    }

    // ---- JSON export が妥当な文字列を返す ----
    {
        const std::string json = perf.export_json();
        PT_ASSERT(json.size() > 32, "json non-trivial");
        PT_ASSERT(json.front() == '{' && json.back() == '}', "json object braces");
        PT_ASSERT(json.find("\"memory\"") != std::string::npos, "json has memory");
        PT_ASSERT(json.find("\"batches\"") != std::string::npos, "json has batches");
        PT_ASSERT(json.find("\"hwCounters\"") != std::string::npos, "json has hwCounters");
    }

    return report("unit_perf_introspection_test");
}
