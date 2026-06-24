#pragma once

#include "pictor/core/types.h"
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// ============================================================
// Perf Introspection — read-only data structures returned by
// PerfQueryAPI (spec/subsystem/perf_introspection.md).
//
// 純データのみ。Vulkan / 内部クラスに依存しない (境界 API は OOP、
// consumer に内部 layout を漏らさない — Pictor CLAUDE.md)。
// ============================================================

namespace pictor {

// ------------------------------------------------------------
// A. Memory layout / cache-efficiency (構造的)
// ------------------------------------------------------------

/// 1 本の SoA stream の物理レイアウト・スナップショット。
struct StreamLayoutInfo {
    std::string name;                   // "bounds", "transforms", ...
    std::string group;                  // "A:culling" / "B:sort" / "C:transform" / "D:meta" / "id"
    PoolType    pool          = PoolType::DYNAMIC;

    size_t      element_size  = 0;      // bytes / element
    size_t      element_count = 0;      // live elements
    size_t      capacity_elements = 0;  // reserved elements
    size_t      used_bytes    = 0;      // element_size * element_count
    size_t      capacity_bytes = 0;     // element_size * capacity_elements

    // 先頭ポインタのアライメント (PoolAllocator は 64B 保証 — その裏取り用)。
    uintptr_t   base_address  = 0;
    size_t      base_alignment = 0;     // base_address を割り切る最大 2^k (4096 で頭打ち)
    bool        base_cache_aligned = false;  // base_address % 64 == 0

    // キャッシュライン充填特性。
    bool        element_straddles_line = false; // 64 % element_size != 0 → 要素がライン跨ぎ
    size_t      cache_lines_touched = 0;        // ceil(used_bytes / 64)
    double      line_utilization_pct = 0.0;     // used_bytes / (cache_lines_touched*64) * 100
    double      elements_per_line = 0.0;        // 64.0 / element_size
};

/// 1 つの ObjectPool の集計レイアウト。
struct PoolLayoutInfo {
    PoolType pool          = PoolType::DYNAMIC;
    uint32_t object_count  = 0;
    size_t   live_bytes    = 0;   // Σ stream.used_bytes
    size_t   reserved_bytes = 0;  // Σ stream.capacity_bytes
    std::vector<StreamLayoutInfo> streams;
};

/// コンパイル時 DoD 不変条件を UI へそのまま供給するためのレコード
/// (編集者が sizeof を再導出しないで済むように)。
struct DoDInvariant {
    std::string name;       // "float4x4", "AABB", "PICTOR_CACHE_LINE_SIZE"
    size_t      actual   = 0;
    size_t      expected = 0;
    bool        ok       = false;
    std::string note;
};

struct MemoryLayoutReport {
    size_t cache_line_size = 0;
    std::vector<PoolLayoutInfo> pools;

    // アロケータ断片化 (reallocate_array は旧配列を解放しないため死にバイトが溜まる)。
    size_t pool_allocator_total_bytes = 0;  // PoolAllocator::total_allocated()
    size_t soa_reserved_bytes = 0;          // Σ 全 stream の capacity_bytes (現生存分)
    size_t soa_used_bytes     = 0;          // Σ 全 stream の used_bytes
    size_t unaccounted_bytes  = 0;          // total - soa_reserved (死に realloc + 非SoA利用)
    double fragmentation_pct  = 0.0;        // unaccounted / total * 100

    std::vector<DoDInvariant> invariants;
};

// ------------------------------------------------------------
// B. Cache traffic model (per-pass)
// ------------------------------------------------------------

struct CacheTrafficInfo {
    std::string pass_name;            // "Culling" / "Sort" / "BatchBuild" / "DataUpdate"
    double      cpu_ms = 0.0;         // 実測 (Profiler FrameStats)
    size_t      streamed_bytes = 0;   // そのパスが読み書きする stream の element_size*count 合計
    double      effective_gbps = 0.0; // streamed_bytes / cpu_ms
    double      line_utilization_pct = 0.0; // touch する stream のライン利用率 (バイト加重平均)
};

// ------------------------------------------------------------
// Batch eligibility (per-object → per-batch)
// ------------------------------------------------------------

/// オブジェクト集合が 1 GPU バッチへ畳めない理由。
enum class BatchSplitReason : uint8_t {
    None = 0,            // 単一バッチ — 完全に畳める
    DifferentMesh,
    DifferentMaterial,
    DifferentShader,
    Transparent,         // 透過は back-to-front ソートで自由に畳めない
    CustomShader,
    DifferentPool,
};

const char* to_string(BatchSplitReason reason);

struct BatchEligibilityGroup {
    PoolType    pool         = PoolType::DYNAMIC;
    MeshHandle  mesh         = INVALID_MESH;
    uint64_t    shader_key   = 0;
    uint32_t    material_key = 0;

    uint32_t    object_count = 0;
    uint32_t    actual_batches = 0;   // この集合が生成した RenderBatch 数
    uint32_t    ideal_batches  = 0;   // 完全に畳めるなら 1
    bool        gpu_batchable  = false; // MDI / instanced へ畳めるか
    std::vector<BatchSplitReason> reasons;
};

struct BatchEligibilityReport {
    uint32_t total_objects  = 0;
    uint32_t total_batches  = 0;
    uint32_t ideal_batches  = 0;          // Σ group.ideal_batches
    double   batch_efficiency_pct = 0.0;  // ideal/actual * 100 (高いほど良く畳めている)
    std::vector<BatchEligibilityGroup> groups;
};

// ------------------------------------------------------------
// Per-batch timing (host-bracketed GPU time)
// ------------------------------------------------------------

struct BatchTimingInfo {
    uint32_t   batch_index  = 0;
    PoolType   pool         = PoolType::DYNAMIC;
    MeshHandle mesh         = INVALID_MESH;
    uint32_t   object_count = 0;     // RenderBatch::count (instanceCount)
    double     gpu_ms       = 0.0;   // BatchGpuTimer 計測値 (未計測なら 0)
    double     per_object_us = 0.0;  // gpu_ms*1000 / object_count
    bool       measured     = false; // host が bracket して実測したか
};

} // namespace pictor
