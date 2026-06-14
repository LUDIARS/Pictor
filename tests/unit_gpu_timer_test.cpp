// GpuTimerManager の region API (const char* / TimestampKind) を headless で検証する。
//
// D-3 (review/2026-06-11): per-frame の `name + "_begin"` 文字列連結を撤廃し、
// region 名は `const char*` リテラルのまま保持、 begin/end は TimestampKind enum で
// 区別する。 ここでは Vulkan を初期化しない (= シミュレーション経路) ので実 GPU
// 時間は出ないが、 region の登録 / 突合 / 公開 API の構造を固定する。

#include "pictor/profiler/gpu_timer.h"
#include "test_common.h"

#include <cstring>

using namespace pictor_test;

int main() {
    pictor::GpuTimerManager gt;

    // 1 フレーム分: 2 region を順に計測する。
    gt.begin_frame(0);
    gt.begin_region("scene_hdr");
    gt.end_region("scene_hdr");
    gt.begin_region("postprocess");
    gt.end_region("postprocess");

    // region あたり begin/end の 2 本 → 計 4 timestamp。
    PT_ASSERT_OP(gt.query_count(), ==, 4u, "2 region = 4 timestamps");

    // collect_results (sim 経路) が resolved_regions_ を確定する。
    gt.collect_results();

    const auto timings = gt.get_all_timings();
    PT_ASSERT_OP(timings.size(), ==, 2u, "2 regions resolved");
    if (timings.size() == 2) {
        // const char* リテラルがそのまま保持されている (連結された別文字列でない)。
        PT_ASSERT(std::strcmp(timings[0].name, "scene_hdr") == 0,  "region 0 名が一致");
        PT_ASSERT(std::strcmp(timings[1].name, "postprocess") == 0, "region 1 名が一致");
    }

    // 既知 region は名前で突合できる (sim 経路では実時間 0ms)。
    PT_ASSERT_OP(gt.get_region_ms("scene_hdr"), ==, 0.0, "sim 経路は実時間 0");
    // 未知 region は 0 を返す (クラッシュしない / nullptr 突合しない)。
    PT_ASSERT_OP(gt.get_region_ms("nonexistent"), ==, 0.0, "未知 region は 0");

    // 次フレームで begin_frame が記録をリセットする。
    gt.begin_frame(1);
    PT_ASSERT_OP(gt.query_count(), ==, 0u, "begin_frame で timestamps クリア");

    return report("unit_gpu_timer_test");
}
