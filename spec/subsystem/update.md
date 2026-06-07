# Update — per-frame オブジェクト更新スケジューラ

> 実装: `include/pictor/update/` (2), `src/update/`

毎フレームの transform/bounds 更新を、オブジェクト数とプール種別に応じて最適な戦略で
ディスパッチする。culling の手前に位置する。

## 構成

| クラス | 役割 |
|---|---|
| `UpdateScheduler` | プール別に更新戦略を自動選択しディスパッチ。motion vector 用に prev_transforms を事前 copy |
| `ThreadPoolDispatcher` | `IJobDispatcher` 実装。64B アライン chunk 分配 + worker pool (auto = cores-1) + atomic queue |

## 更新戦略 (自動選択)

```
select_strategy(type, count):
  STATIC                                  → NONE        (更新しない)
  GPU_DRIVEN & compute shader あり          → GPU_COMPUTE
  DYNAMIC & count > nt_threshold & NT 有効  → CPU_PARALLEL_NT
  DYNAMIC その他                           → CPU_PARALLEL
```

- **CPU_PARALLEL**: ThreadPoolDispatcher で chunk (既定 16384) 並列、callback が `transforms[]`/`bounds[]` を直接書込
- **CPU_PARALLEL_NT**: 上記 + AVX2 non-temporal store (`_mm256_stream_ps` + `_mm_sfence`) で大バッチの LLC 汚染回避
- **GPU_COMPUTE**: `ComputeUpdateParams` (dt/total_time/frame/gravity) を stage、実 dispatch は `GPUDrivenPipeline::execute()` 内で非同期

## 主要 API

```cpp
void update(float delta_time);                    // プール毎に戦略選択 + dispatch
void set_update_callback(IUpdateCallback*);
void set_job_dispatcher(IJobDispatcher*);
UpdateStrategy strategy_for(PoolType) const;
// ThreadPoolDispatcher
void dispatch(uint32_t count, uint32_t chunk_size, JobFunction);
void wait_all();
```

`UpdateConfig`: chunk_size=16384 / worker_threads=0(auto) / nt_store_enabled=true / nt_store_threshold=10000。

## 位置 / 依存

フレームでは **culling の直前**: pre-update で prev_transforms を copy (motion vector) → 戦略別更新 → 更新後の transform/bounds を culling と batch が次段で使う。依存: `scene/{scene_registry,object_pool}.h`、`<thread>/<atomic>`、`<immintrin.h>` (AVX2)。関連: [scene.md](scene.md) / [gpu.md](gpu.md)。
