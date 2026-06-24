#include "pictor/profiler/perf_query_api.h"

#include "pictor/scene/scene_registry.h"
#include "pictor/memory/memory_subsystem.h"
#include "pictor/batch/batch_builder.h"
#include "pictor/profiler/profiler.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <sstream>

// Win32 GDI (wingdi.h) は `TRANSPARENT` を #define 1 する。透過的に取り込まれると
// `ObjectFlags::TRANSPARENT` が `ObjectFlags::1` に化けてビルドが壊れるので外す。
#ifdef TRANSPARENT
#undef TRANSPARENT
#endif

namespace pictor {

// ============================================================
// 共通ヘルパ
// ============================================================

const char* to_string(BatchSplitReason reason) {
    switch (reason) {
        case BatchSplitReason::Mergeable:         return "Mergeable";
        case BatchSplitReason::DifferentMesh:     return "DifferentMesh";
        case BatchSplitReason::DifferentMaterial: return "DifferentMaterial";
        case BatchSplitReason::DifferentShader:   return "DifferentShader";
        case BatchSplitReason::Transparent:       return "Transparent";
        case BatchSplitReason::CustomShader:      return "CustomShader";
        case BatchSplitReason::DifferentPool:     return "DifferentPool";
    }
    return "Unknown";
}

namespace {

constexpr size_t kCacheLine = PICTOR_CACHE_LINE_SIZE > 0 ? PICTOR_CACHE_LINE_SIZE : 64;

const char* pool_name(PoolType t) {
    switch (t) {
        case PoolType::STATIC:     return "STATIC";
        case PoolType::DYNAMIC:    return "DYNAMIC";
        case PoolType::GPU_DRIVEN: return "GPU_DRIVEN";
    }
    return "?";
}

/// アドレスを割り切る最大の 2^k (4096 で頭打ち)。0 アドレスは 0。
size_t address_alignment(uintptr_t addr) {
    if (addr == 0) return 0;
    size_t align = 1;
    while (align < 4096 && (addr % (align * 2)) == 0) align *= 2;
    return align;
}

struct LineMetric {
    size_t used  = 0;
    size_t lines = 0;
    double util  = 0.0; // %
};

LineMetric line_metric(size_t element_size, size_t count) {
    LineMetric m;
    m.used  = element_size * count;
    m.lines = (m.used + kCacheLine - 1) / kCacheLine;
    m.util  = m.lines ? (static_cast<double>(m.used) / (m.lines * static_cast<double>(kCacheLine))) * 100.0 : 0.0;
    return m;
}

// 簡易 JSON helper -------------------------------------------------

void json_escape(std::ostringstream& os, const std::string& s) {
    os << '"';
    for (char c : s) {
        switch (c) {
            case '"':  os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\n': os << "\\n";  break;
            case '\t': os << "\\t";  break;
            default:   os << c;      break;
        }
    }
    os << '"';
}

void json_num(std::ostringstream& os, double v) {
    if (std::isnan(v) || std::isinf(v)) { os << "null"; return; }
    os << v;
}

} // namespace

// ============================================================
// 構築 / 破棄
// ============================================================

PerfQueryAPI::PerfQueryAPI(const SceneRegistry& scene,
                           const MemorySubsystem& memory,
                           const BatchBuilder& batches,
                           const Profiler& profiler)
    : scene_(scene), memory_(memory), batches_(batches), profiler_(profiler) {}

PerfQueryAPI::~PerfQueryAPI() = default;

// ============================================================
// A. メモリレイアウト / キャッシュ効率
// ============================================================

MemoryLayoutReport PerfQueryAPI::memory_layout_report() const {
    MemoryLayoutReport report;
    report.cache_line_size = kCacheLine;

    const std::array<PoolType, 3> pools = {
        PoolType::STATIC, PoolType::DYNAMIC, PoolType::GPU_DRIVEN
    };

    size_t soa_reserved = 0;
    size_t soa_used = 0;

    for (PoolType pt : pools) {
        const ObjectPool& pool = scene_.pool(pt);
        PoolLayoutInfo pinfo;
        pinfo.pool = pt;
        pinfo.object_count = pool.count();

        for (const auto& sv : pool.stream_views()) {
            StreamLayoutInfo s;
            s.name              = sv.name;
            s.group             = sv.group;
            s.pool              = pt;
            s.element_size      = sv.element_size;
            s.element_count     = sv.count;
            s.capacity_elements = sv.capacity;
            s.used_bytes        = sv.element_size * sv.count;
            s.capacity_bytes    = sv.element_size * sv.capacity;

            s.base_address       = reinterpret_cast<uintptr_t>(sv.base);
            s.base_alignment     = address_alignment(s.base_address);
            s.base_cache_aligned = (s.base_address != 0) && (s.base_address % kCacheLine == 0);

            // 要素がキャッシュラインを跨ぐ = 要素サイズが 64 の約数でない
            // (例: AABB=24B)。約数なら 1 要素は必ず 1 ライン内に収まる。
            s.element_straddles_line =
                (sv.element_size != 0) && (kCacheLine % sv.element_size != 0)
                && (sv.element_size < kCacheLine);
            // 64 の倍数要素 (例 float4x4=64) も「跨がない」(整列している)。
            if (sv.element_size != 0 && sv.element_size % kCacheLine == 0) {
                s.element_straddles_line = false;
            }

            const LineMetric lm = line_metric(sv.element_size, sv.count);
            s.cache_lines_touched  = lm.lines;
            s.line_utilization_pct = lm.util;
            s.elements_per_line    = sv.element_size ? static_cast<double>(kCacheLine) / sv.element_size : 0.0;

            pinfo.live_bytes     += s.used_bytes;
            pinfo.reserved_bytes += s.capacity_bytes;
            soa_used     += s.used_bytes;
            soa_reserved += s.capacity_bytes;

            pinfo.streams.push_back(std::move(s));
        }
        report.pools.push_back(std::move(pinfo));
    }

    // アロケータ断片化 — reallocate_array が旧配列を解放しないため、
    // total_allocated は現生存 SoA reserved を上回る (死にバイト + 非 SoA 利用)。
    const MemorySubsystem::Stats mstats = memory_.get_stats();
    report.pool_allocator_total_bytes = mstats.pool_allocated;
    report.soa_reserved_bytes = soa_reserved;
    report.soa_used_bytes     = soa_used;
    report.unaccounted_bytes  = (mstats.pool_allocated > soa_reserved)
        ? (mstats.pool_allocated - soa_reserved) : 0;
    report.fragmentation_pct  = mstats.pool_allocated
        ? (static_cast<double>(report.unaccounted_bytes) / mstats.pool_allocated) * 100.0
        : 0.0;

    // DoD 不変条件 (コンパイル時の事実を UI へそのまま供給)。
    auto inv = [](const char* name, size_t actual, size_t expected, const char* note) {
        DoDInvariant d;
        d.name = name; d.actual = actual; d.expected = expected;
        d.ok = (actual == expected); d.note = note;
        return d;
    };
    report.invariants.push_back(inv("float4x4", sizeof(float4x4), 64,
        "1 transform = 1 cache line (alignas(64))"));
    report.invariants.push_back(inv("AABB", sizeof(AABB), 24,
        "tight-pack。24B は 64 の非約数 → bounds stream は要素ライン跨ぎあり"));
    report.invariants.push_back(inv("float3", sizeof(float3), 12,
        "12B tight-pack (SIMD 整列ではない)"));
    report.invariants.push_back(inv("PICTOR_CACHE_LINE_SIZE", kCacheLine, 64,
        "x86/x64・Apple Silicon は 64。-D で上書き可"));

    return report;
}

// ============================================================
// B. パス毎キャッシュトラフィック (モデル推定)
// ============================================================

std::vector<CacheTrafficInfo> PerfQueryAPI::cache_traffic() const {
    const FrameStats& fs = profiler_.get_frame_stats();

    const uint32_t total =
        scene_.static_pool().count() +
        scene_.dynamic_pool().count() +
        scene_.gpu_driven_pool().count();
    const uint32_t dyn = scene_.dynamic_pool().count();

    // 各パスが触る stream の要素サイズ合計 × 対象オブジェクト数。
    struct PassDef {
        const char* name;
        double      cpu_ms;
        size_t      bytes_per_object;  // そのパスが読み書きする stream の要素サイズ和
        uint32_t    object_count;
    };

    const std::array<PassDef, 4> passes = {{
        // DataUpdate: transforms (read+write) + prev_transforms (write)
        {"DataUpdate", fs.data_update_ms, sizeof(float4x4) * 2, dyn},
        // Culling: bounds (read) + visibility_flags (write)
        {"Culling", fs.culling_ms, sizeof(AABB) + sizeof(uint8_t), total},
        // Sort: shader_keys + sort_keys (read/write) + index (uint32)
        {"Sort", fs.sort_ms, sizeof(uint64_t) * 2 + sizeof(uint32_t), total},
        // BatchBuild: shader_keys + material_keys + mesh_handles (read)
        {"BatchBuild", fs.batch_build_ms,
            sizeof(uint64_t) + sizeof(uint32_t) + sizeof(MeshHandle), total},
    }};

    std::vector<CacheTrafficInfo> out;
    out.reserve(passes.size());
    for (const auto& p : passes) {
        CacheTrafficInfo c;
        c.pass_name      = p.name;
        c.cpu_ms         = p.cpu_ms;
        c.streamed_bytes = p.bytes_per_object * p.object_count;
        // 実効帯域 = bytes / 秒。cpu_ms が 0 (未計測) なら 0。
        c.effective_gbps = (p.cpu_ms > 0.0)
            ? (static_cast<double>(c.streamed_bytes) / (p.cpu_ms / 1000.0)) / 1.0e9
            : 0.0;
        // SoA は基本詰まっているのでライン利用率はパス全体の bytes から概算。
        const LineMetric lm = line_metric(1, c.streamed_bytes); // 1B 要素換算 = 連続バイト
        c.line_utilization_pct = lm.util;
        out.push_back(std::move(c));
    }
    return out;
}

// ============================================================
// バッチ適格性 (per-object → per-batch)
// ============================================================

BatchEligibilityReport PerfQueryAPI::batch_eligibility_report() const {
    BatchEligibilityReport report;

    // (pool, mesh, shaderKey, materialKey) でグルーピング。
    struct Key {
        PoolType pool; MeshHandle mesh; uint64_t shader; uint32_t material;
        bool operator<(const Key& o) const {
            if (pool != o.pool) return pool < o.pool;
            if (mesh != o.mesh) return mesh < o.mesh;
            if (shader != o.shader) return shader < o.shader;
            return material < o.material;
        }
    };
    struct Agg {
        uint32_t count = 0;
        bool any_transparent = false;
        bool any_custom = false;
    };
    std::map<Key, Agg> groups;

    const std::array<PoolType, 3> pools = {
        PoolType::STATIC, PoolType::DYNAMIC, PoolType::GPU_DRIVEN
    };
    for (PoolType pt : pools) {
        const ObjectPool& pool = scene_.pool(pt);
        const uint32_t n = pool.count();
        const auto& meshes    = pool.mesh_handles();
        const auto& shaders   = pool.shader_keys();
        const auto& materials = pool.material_keys();
        const auto& flags     = pool.flags();
        for (uint32_t i = 0; i < n; ++i) {
            Key k{pt, meshes[i], shaders[i], materials[i]};
            Agg& a = groups[k];
            a.count++;
            if (flags[i] & ObjectFlags::TRANSPARENT) a.any_transparent = true;
            if (ShaderKey::is_custom(shaders[i]))     a.any_custom = true;
        }
    }

    // 実バッチ数を (mesh, shaderKey, materialKey) で照合。
    auto actual_batches_for = [&](MeshHandle mesh, uint64_t shader, uint32_t material) -> uint32_t {
        uint32_t c = 0;
        for (const RenderBatch& b : batches_.batches()) {
            if (b.mesh == mesh && b.shaderKey == shader && b.materialKey == material) c++;
        }
        return c;
    };

    uint32_t total_objects = 0;
    uint32_t ideal_total = 0;
    for (const auto& [k, a] : groups) {
        BatchEligibilityGroup g;
        g.pool         = k.pool;
        g.mesh         = k.mesh;
        g.shader_key   = k.shader;
        g.material_key = k.material;
        g.object_count = a.count;
        g.actual_batches = actual_batches_for(k.mesh, k.shader, k.material);

        // ideal: 単体なら 1、透過は順序制約で畳めない (= object_count)、それ以外は 1。
        if (a.count <= 1) {
            g.ideal_batches = a.count;
            g.gpu_batchable = false;
            g.reasons.push_back(BatchSplitReason::Mergeable);
        } else if (a.any_transparent) {
            g.ideal_batches = a.count;
            g.gpu_batchable = false;
            g.reasons.push_back(BatchSplitReason::Transparent);
        } else {
            g.ideal_batches = 1;
            g.gpu_batchable = true;
            if (a.any_custom) g.reasons.push_back(BatchSplitReason::CustomShader);
            else              g.reasons.push_back(BatchSplitReason::Mergeable);
        }

        total_objects += a.count;
        ideal_total   += g.ideal_batches;
        report.groups.push_back(std::move(g));
    }

    report.total_objects = total_objects;
    report.total_batches = static_cast<uint32_t>(batches_.batches().size());
    report.ideal_batches = ideal_total;
    report.batch_efficiency_pct = report.total_batches
        ? (static_cast<double>(ideal_total) / report.total_batches) * 100.0
        : 0.0;

    return report;
}

// ============================================================
// per-batch GPU 時間 / HW カウンタ
// ============================================================

void PerfQueryAPI::set_batch_timings(std::vector<BatchTimingInfo> timings) {
    batch_timings_ = std::move(timings);
}

HwCounterSample PerfQueryAPI::hw_counters() const {
    if (!hw_provider_) {
        HwCounterSample s;
        s.available = false;
        s.source = "disabled";
        s.unavailable_reason = "HW counter provider 未注入 (set_hardware_counter_provider)";
        return s;
    }
    hw_provider_->begin_sample();
    return hw_provider_->end_sample();
}

// ============================================================
// JSON export (spec §5 契約)
// ============================================================

std::string PerfQueryAPI::export_memory_json() const {
    const MemoryLayoutReport r = memory_layout_report();
    std::ostringstream os;
    os << "{";
    os << "\"cacheLineSize\":" << r.cache_line_size;
    os << ",\"poolAllocatorTotalBytes\":" << r.pool_allocator_total_bytes;
    os << ",\"soaReservedBytes\":" << r.soa_reserved_bytes;
    os << ",\"soaUsedBytes\":" << r.soa_used_bytes;
    os << ",\"unaccountedBytes\":" << r.unaccounted_bytes;
    os << ",\"fragmentationPct\":"; json_num(os, r.fragmentation_pct);

    os << ",\"invariants\":[";
    for (size_t i = 0; i < r.invariants.size(); ++i) {
        const auto& d = r.invariants[i];
        if (i) os << ",";
        os << "{\"name\":"; json_escape(os, d.name);
        os << ",\"actual\":" << d.actual << ",\"expected\":" << d.expected
           << ",\"ok\":" << (d.ok ? "true" : "false")
           << ",\"note\":"; json_escape(os, d.note); os << "}";
    }
    os << "]";

    os << ",\"pools\":[";
    for (size_t pi = 0; pi < r.pools.size(); ++pi) {
        const auto& p = r.pools[pi];
        if (pi) os << ",";
        os << "{\"pool\":\"" << pool_name(p.pool) << "\""
           << ",\"objectCount\":" << p.object_count
           << ",\"liveBytes\":" << p.live_bytes
           << ",\"reservedBytes\":" << p.reserved_bytes
           << ",\"streams\":[";
        for (size_t si = 0; si < p.streams.size(); ++si) {
            const auto& s = p.streams[si];
            if (si) os << ",";
            os << "{\"name\":"; json_escape(os, s.name);
            os << ",\"group\":"; json_escape(os, s.group);
            os << ",\"elementSize\":" << s.element_size
               << ",\"elementCount\":" << s.element_count
               << ",\"usedBytes\":" << s.used_bytes
               << ",\"capacityBytes\":" << s.capacity_bytes
               << ",\"baseAlignment\":" << s.base_alignment
               << ",\"baseCacheAligned\":" << (s.base_cache_aligned ? "true" : "false")
               << ",\"straddlesLine\":" << (s.element_straddles_line ? "true" : "false")
               << ",\"cacheLinesTouched\":" << s.cache_lines_touched
               << ",\"lineUtilizationPct\":"; json_num(os, s.line_utilization_pct);
            os << ",\"elementsPerLine\":"; json_num(os, s.elements_per_line);
            os << "}";
        }
        os << "]}";
    }
    os << "]";
    os << "}";
    return os.str();
}

std::string PerfQueryAPI::export_batches_json() const {
    const BatchEligibilityReport r = batch_eligibility_report();
    std::ostringstream os;
    os << "{";
    os << "\"totalObjects\":" << r.total_objects
       << ",\"totalBatches\":" << r.total_batches
       << ",\"idealBatches\":" << r.ideal_batches
       << ",\"batchEfficiencyPct\":"; json_num(os, r.batch_efficiency_pct);
    os << ",\"groups\":[";
    for (size_t i = 0; i < r.groups.size(); ++i) {
        const auto& g = r.groups[i];
        if (i) os << ",";
        os << "{\"pool\":\"" << pool_name(g.pool) << "\""
           << ",\"mesh\":" << g.mesh
           << ",\"shaderKey\":" << g.shader_key
           << ",\"materialKey\":" << g.material_key
           << ",\"objectCount\":" << g.object_count
           << ",\"actualBatches\":" << g.actual_batches
           << ",\"idealBatches\":" << g.ideal_batches
           << ",\"gpuBatchable\":" << (g.gpu_batchable ? "true" : "false")
           << ",\"reasons\":[";
        for (size_t ri = 0; ri < g.reasons.size(); ++ri) {
            if (ri) os << ",";
            os << "\"" << to_string(g.reasons[ri]) << "\"";
        }
        os << "]}";
    }
    os << "]}";
    return os.str();
}

std::string PerfQueryAPI::export_json() const {
    std::ostringstream os;
    os << "{";
    os << "\"memory\":" << export_memory_json();

    os << ",\"cacheTraffic\":[";
    const auto traffic = cache_traffic();
    for (size_t i = 0; i < traffic.size(); ++i) {
        const auto& c = traffic[i];
        if (i) os << ",";
        os << "{\"pass\":"; json_escape(os, c.pass_name);
        os << ",\"cpuMs\":"; json_num(os, c.cpu_ms);
        os << ",\"streamedBytes\":" << c.streamed_bytes;
        os << ",\"gbps\":"; json_num(os, c.effective_gbps);
        os << ",\"lineUtilPct\":"; json_num(os, c.line_utilization_pct);
        os << "}";
    }
    os << "]";

    os << ",\"batches\":" << export_batches_json();

    os << ",\"batchTimings\":[";
    for (size_t i = 0; i < batch_timings_.size(); ++i) {
        const auto& t = batch_timings_[i];
        if (i) os << ",";
        os << "{\"batchIndex\":" << t.batch_index
           << ",\"pool\":\"" << pool_name(t.pool) << "\""
           << ",\"mesh\":" << t.mesh
           << ",\"objectCount\":" << t.object_count
           << ",\"gpuMs\":"; json_num(os, t.gpu_ms);
        os << ",\"perObjectUs\":"; json_num(os, t.per_object_us);
        os << ",\"measured\":" << (t.measured ? "true" : "false") << "}";
    }
    os << "]";

    const HwCounterSample hw = hw_counters();
    os << ",\"hwCounters\":{"
       << "\"available\":" << (hw.available ? "true" : "false")
       << ",\"source\":"; json_escape(os, hw.source);
    os << ",\"reason\":"; json_escape(os, hw.unavailable_reason);
    os << ",\"l2HitRatio\":"; json_num(os, hw.l2_hit_ratio);
    os << ",\"l3HitRatio\":"; json_num(os, hw.l3_hit_ratio);
    os << ",\"l2Misses\":" << hw.l2_misses
       << ",\"l3Misses\":" << hw.l3_misses
       << ",\"bytesReadMC\":"; json_num(os, hw.bytes_read_mc);
    os << ",\"bytesWriteMC\":"; json_num(os, hw.bytes_write_mc);
    os << ",\"ipc\":"; json_num(os, hw.ipc);
    os << "}";

    os << "}";
    return os.str();
}

} // namespace pictor
