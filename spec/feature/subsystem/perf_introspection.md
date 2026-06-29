# Perf Introspection — per-object/batch 性能 & DoD キャッシュ可視化

> 対象: `include/pictor/profiler/perf_introspection.h` /
> `perf_query_api.h` / `hardware_counters.h` / `batch_gpu_timer.h`。
> consumer は Ergo の `pictor_perf` プラグイン (web)。

## 1. 目的と重視点 (RULE_CODE 第 I 部)

Ergo から Pictor を **per-object / per-batch** で性能観測し、かつ
**DoD のメモリアライメントが効いているか / キャッシュ効率が落ちていないか**
を視認可能にする。Pictor は最下層なので上位 (Ergo) を取り込まず
([[feedback_pictor_no_upper_dep]])、`DataQueryAPI` と同じ **read-only query API**
パターンで「内部 layout を漏らさず観測値だけ返す」(境界 API は OOP)。

重視点:
1. **正直さ (無言フォールバック禁止 [[feedback_no_silent_fallback]])** — 計測できない値を
   でっち上げない。per-batch GPU 時間は Pictor が draw を発行しない host-driven 経路では
   `gpu_ms = 0` のまま返し「未計測」と明示する。HW カウンタは取得不能なら
   `available=false` + 理由文字列を返す。
2. **hot path を汚さない** — 観測 API は既存状態 (pools / Profiler / BatchBuilder /
   allocator stats) を**読むだけ**。レンダーループに分岐を増やさない。
3. **DoD 不変条件を機械で見せる** — `sizeof(float4x4)==64` 等を `DoDInvariant` として
   そのまま UI へ供給し、編集者が再導出しなくて済む。

## 2. 三層のキャッシュ可視化

| 層 | 何を見るか | 取得方法 | HW 依存 |
|---|---|---|---|
| **A 構造的** | stream 毎の要素サイズ / 先頭ptr アライメント / used・capacity / **キャッシュライン跨ぎ** / ライン利用率 / アロケータ断片化 | `MemoryLayoutReport` (pools/allocator を走査) | なし |
| **B モデル推定** | パス毎の streamed bytes と実測 ms から **実効帯域 GB/s** とライン利用率 | `CacheTrafficInfo` (Profiler FrameStats × stream サイズ) | なし |
| **C ハードウェア** | 真の L2/L3 ヒット率・DRAM 帯域・IPC | `IHardwareCounterProvider` (Intel PCM) | あり (opt-in) |

A+B は常時・全環境。C は `PICTOR_ENABLE_HW_COUNTERS=ON` かつ PCM 連携時のみ実値、
それ以外は null provider が理由付きで `available=false`。

## 3. per-object → per-batch の読み替え (設計判断)

Pictor は SoA + バッチ + GPU-driven で、**個別オブジェクトの draw call は存在しない**
(static=MDI / dynamic=instanced / gpu-driven=GPU 生成)。よって:

- **GPU 時間はバッチ単位** (`BatchTimingInfo`)。1 オブジェクト按分 = `gpu_ms / object_count`。
- **バッチ適格性** (`BatchEligibilityReport`): `(pool, mesh, shaderKey, materialKey)` で
  グルーピングし `should_merge` 相当の判定で「N 個 → 1 バッチにできる / M バッチに割れた・
  その理由 (mesh差/material差/transparent/customShader/pool差)」を出す。これが
  「GPU でバッチ対象か」の答え。
- per-batch GPU 時間は `BatchGpuTimer` で **host が draw cmd を bracket** して計測する
  (Pictor の managed path は host-driven のため。§1 の正直さ原則)。

## 4. API 配置 (SRP / ファイル分割)

| ファイル | 責務 |
|---|---|
| `perf_introspection.h` | 戻り値の純データ struct 群 (Vulkan 非依存) |
| `hardware_counters.h/.cpp` | C 層: `IHardwareCounterProvider` + Null / PCM 実装 + factory |
| `batch_gpu_timer.h/.cpp` | per-batch GPU 時間の host-bracket ヘルパ (`GpuTimerManager` ラップ) |
| `perf_query_api.h/.cpp` | A/B/batch を集約する read-only facade `PerfQueryAPI` + JSON export |
| `scene/object_pool.h` | `StreamView` + `stream_views()` (stream の物理 introspection 口) |
| `core/pictor_renderer.h` | `create_perf_query_api()` / `batch_builder()` 公開口 |

## 5. JSON 契約 (Ergo plugin が消費)

`PerfQueryAPI::export_json()` が 1 オブジェクトを返す:
```jsonc
{
  "memory": { "cacheLineSize":64, "fragmentationPct":..., "invariants":[...], "pools":[...] },
  "cacheTraffic": [ {"pass":"Culling","cpuMs":..,"streamedBytes":..,"gbps":..,"lineUtilPct":..} ],
  "batches": { "totalObjects":.., "totalBatches":.., "batchEfficiencyPct":.., "groups":[...] },
  "batchTimings": [ {"batchIndex":0,"mesh":..,"objectCount":..,"gpuMs":..,"perObjectUs":..} ],
  "hwCounters": { "available":false, "source":"disabled", "reason":"PICTOR_ENABLE_HW_COUNTERS=OFF" }
}
```

## 6. テスト (RULE_TEST)

`tests/unit_perf_introspection_test.cpp` (headless, Vulkan 不要):
- stream 先頭 ptr が 64B 整列していること (PoolAllocator 保証の裏取り)。
- `AABB` ストリームの `element_straddles_line == true` (24B = 64 の非約数) を検出すること。
- `transforms` (float4x4=64B) は跨がず `elements_per_line==1`、`line_utilization==100%`。
- 同一 mesh/material の N オブジェクトが 1 グループ・`gpu_batchable==true`、
  mesh 違いで別グループ・理由 `DifferentMesh` になること。
- HW counter null provider が `available=false` + 非空 reason を返すこと。
- DoD invariants (`float4x4==64`, `AABB==24`) が全て `ok==true`。
