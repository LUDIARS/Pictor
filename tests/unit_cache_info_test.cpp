/// Cache-line portability unit test (headless)。
/// spec/subsystem/cache_portability.md を裏取りする。

#include "pictor/core/cache_info.h"
#include "pictor/core/types.h"
#include "test_common.h"

using namespace pictor;
using namespace pictor_test;

static bool is_pow2(size_t v) { return v != 0 && (v & (v - 1)) == 0; }

int main() {
    // 1. concern 分離: ライン幅を 128 にしても float4x4 は 64B のまま。
    PT_ASSERT_OP(sizeof(float4x4), ==, 64u, "float4x4 stays 64B regardless of line size");
    PT_ASSERT_OP(alignof(float4x4), ==, 16u, "float4x4 uses SIMD (16B) packing align");
    PT_ASSERT_OP(sizeof(AABB), ==, 24u, "AABB tight-packed 24B");

    // 2. ビルド時ライン幅は妥当な 2 のべき (32/64/128)。
    PT_ASSERT(is_pow2(PICTOR_CACHE_LINE_SIZE), "compiled line size is power of two");
    PT_ASSERT(PICTOR_CACHE_LINE_SIZE >= 32 && PICTOR_CACHE_LINE_SIZE <= 256,
              "compiled line size in [32,256]");

    // 3. ランタイム検出。
    const CacheLineInfo info = query_cache_line_info();
    PT_ASSERT_OP(info.compiled_line_size, ==, (size_t)PICTOR_CACHE_LINE_SIZE,
                 "info.compiled matches macro");
    PT_ASSERT(!info.arch.empty(), "arch string non-empty");
    PT_ASSERT(!info.source.empty(), "source string non-empty");

    // 検出できたなら妥当な 2 のべき。0 (不明) なら matches=true 扱い。
    if (info.runtime_line_size != 0) {
        PT_ASSERT(is_pow2(info.runtime_line_size), "runtime line size power of two");
        PT_ASSERT(info.runtime_line_size >= 32 && info.runtime_line_size <= 256,
                  "runtime line size in [32,256]");
        PT_ASSERT_OP(info.matches, ==, (info.runtime_line_size == info.compiled_line_size),
                     "matches reflects runtime vs compiled");
    } else {
        PT_ASSERT(info.matches, "unknown runtime → matches=true (no false warning)");
    }

    // 4. 現在のビルドホスト (x86-64 CI / dev) では 64B のはず — 一致を期待。
    //    (Apple 128B 機では別 expectation だが、本 CI は x86-64。)
#if defined(__x86_64__) || defined(_M_X64)
    PT_ASSERT_OP(PICTOR_CACHE_LINE_SIZE, ==, 64u, "x86-64 compiles to 64B line");
#endif

    return report("unit_cache_info_test");
}
