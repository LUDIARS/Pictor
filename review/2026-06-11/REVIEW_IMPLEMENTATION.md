# 実装評価 — Pictor (2026-06-11, Fable 診断)

対象: メモリ管理 (memory/) ・並行性 (update/) ・データ構造 (scene/ batch/ culling/ gpu/)。
全指摘は実コード読解で検証済み (HEAD: 4789398)。

---

## Critical

### C-1. ThreadPoolDispatcher の lost wakeup による `wait_all()` 永久ハング
**`src/update/job_dispatcher.cpp:49-54, 76-78`**

`wait_all()` の述語は `pending_tasks_` (atomic) を mutex 保護下で評価するが、worker 側のデクリメント + `cv_done_.notify_all()` は **mutex を取らずに**実行される:

```cpp
if (pending_tasks_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    cv_done_.notify_all();   // mutex 非保持
}
```

**故障シナリオ**: waiter が mutex 保持中に述語評価 (`pending==1` → false) → `wait()` でブロックする直前に worker が `fetch_sub→0` + `notify_all()` → 通知消失 → 永久ブロック。`UpdateScheduler::update()` が毎フレーム dispatch+wait を行う (`pictor_renderer.cpp:165` 経由) ため、60fps で毎フレーム競合ウィンドウを踏み、いずれレンダースレッドがハングする。
**修正**: worker 側でデクリメント〜notify を `std::lock_guard<std::mutex>` 下で行う (条件変数の状態変更は同じ mutex 下で、の原則)。

## High

### H-1. BatchBuilder — static バッチが 2 フレーム目以降消失
**`src/batch/batch_builder.cpp:10-23`** — `build()` 冒頭で `batches_.clear()` するが `build_static()` は `dirty_[STATIC]` 時のみ実行。フレーム 1 で dirty が落ちると、以降 static pool のバッチが再生成されず**静的オブジェクトが描画から消える**。
**修正**: static バッチを別コンテナにキャッシュし `build()` で毎回マージ (dirty 時のみ再構築)。

### H-2. FrameAllocator move 代入での解放関数ミスマッチ (ヒープ破壊)
**`src/memory/frame_allocator.cpp:94-102`** — デストラクタ (63-79 行) は `PICTOR_LARGE_PAGES` 時 `VirtualFree`/`munmap` を使うが、move 代入は無条件に `_aligned_free`/`std::free`。large pages ビルドで mmap 領域を `free()` に渡す UB。
**修正**: 解放処理を `release_()` に一本化し dtor / move 代入の双方から呼ぶ。

### H-3. GPU リング(称)バッファに in-flight フレーム保護なし + 虚偽の fence コメント
**`src/memory/memory_subsystem.cpp:17-23` / `src/memory/gpu_memory_allocator.cpp:125-135` / `src/core/pictor_renderer.cpp:142-144`**

`begin_frame()` が毎フレーム instance/staging プール全体を即時回収。flight_count=3 で前フレームのコマンドバッファが GPU 参照中でも同領域が再割当てされ得る。renderer 側コメントは「§12: **Fence wait** + reset」と謳うが、fence 待機は**どこにも存在しない**。「ring」と称するが実体は全域リセット。`GPUBufferManager::free_soa_buffers` / `resize_soa_buffers` (`gpu_buffer_manager.cpp:34-53`) も即時解放で同症状。
**修正**: instance/staging プールを flight 数だけ多重化 (FlightFrameAllocator と同方式) するか、fence 完了確認後に回収。

## Medium

| # | 指摘 | 場所 | 要点 |
|---|------|------|------|
| M-1 | SoAStream::swap_and_pop / ObjectPool::remove の size==0 アンダーフロー | `soa_stream.h:57-61` / `object_pool.cpp:55-61` | `size_ - 1` が SIZE_MAX にラップ、`object_ids_[last]` OOB read。public API なのに `index >= count` 検証なし |
| M-2 | GPUBufferManager::mark_dirty — 既マーク chunk の範囲拡張が失われる | `gpu_buffer_manager.cpp:85-108` | found 時に `end_object` を max 更新せず → 未アップロード領域 (GPU stale データ)。線形探索も O(n×m) |
| M-3 | `clear_dirty()` が `dirty_chunk_count_` をリセットしない | `gpu_buffer_manager.cpp:110-114` | カウンタ単調増加で `should_full_copy()` が常時 true 化 |
| M-4 | GpuMemoryAllocator::free — 二重 free / ring リセット後 free でフリーリスト破壊 | `gpu_memory_allocator.cpp:86-123` | 重複ブロック挿入 + `used` アンダーフロー → 同領域の二重貸出。`is_ring` フラグは**どこからも参照されていない** |
| M-5 | BatchBuilder::sorted_indices_ — FrameAllocator メモリへの跨フレームダングリング | `batch_builder.cpp:148-156` | dynamic count=0 のフレームで前フレームのリセット済み領域を指したまま。public getter が use-after-reset を露出 |
| M-6 | RadixSort — frame メモリ枯渇時に未ソートのまま silent 成功 | `radix_sort.cpp:18-19` → `batch_builder.cpp:75-91` | culled (key=UINT64_MAX) 末尾前提が崩れ、カリング済みがバッチ混入。bool を返し fallback (std::sort) すべき |
| M-7 | dispatch の `pending_tasks_.store()` 上書き | `job_dispatcher.cpp:44` | 残タスクありで再 dispatch すると残数を上書き。`fetch_add` に |
| M-8 | `hardware_concurrency()==0` で uint32 ラップ | `job_dispatcher.cpp:8-11` / `update_scheduler.cpp:19-22` | `0 - 1 = 0xFFFFFFFF` スレッド生成を試行 |
| M-9 | BVH 再構築のたびに PoolAllocator が単調成長 | `flat_bvh.cpp:20-23, 284-285` / `pool_allocator.cpp:69-80` | reallocate は旧領域を回収しない設計のため、定期 rebuild 運用で unbounded growth。専用 allocator + clear() を |
| M-10 | FlatBVH トラバーサルの固定 64 段スタックで部分木を無音で破棄 | `flat_bvh.cpp:208, 230-234, 247, 266-267` | 可視オブジェクトが消える。飽和時は線形走査フォールバックを |
| M-11 | WorldPartition — swap-and-pop 後のインデックス不整合 | `world_partition.cpp` / `scene_registry.cpp:39-57` | pool index 付け替えが partition に伝播せず、別オブジェクトの可視性を誤判定 (OOB はガード済み) |

## Low

- **L-1** `frame_allocator.cpp:131-134`: `peak_` の非 atomic 複数スレッド書込み (規格上データレース)。relaxed CAS で安価に解消。
- **L-2** `frame_allocator.cpp:53`: `std::aligned_alloc(64, capacity)` — capacity が 64 の倍数でないと規格外 (PoolAllocator 側は正しく切り上げており対照的)。
- **L-3** `update_scheduler.cpp:114-134`: NT store パスがコメント「Update to temporary first」に反し、callback が書いた**同一アドレス**を load → `_mm256_stream_ps` し直すだけ。キャッシュ汚染回避効果ゼロで sfence コストのみ。テンポラリ経由に直すか Level 2 を削除。
- **L-4** `batch_builder.cpp:175-184`: `get_stats()` が registry 総数とバッチ内 count を二重加算。
- **L-5** `scene_registry.cpp:89-97`: `change_pool()` が `customShader` を復元せず INVALID_SHADER に落とす (プール移動でカスタムシェーダ描画が消える)。
- **L-6** `flat_bvh.cpp:160-199`: `refit()` が `current_cost_` を更新せず `needs_rebuild()` が恒久 false (品質劣化検知が死んでいる)。
- **L-7** `gpu_driven_pipeline.cpp:25-36`: `GpuAllocation::valid` 未検査。SSBO プール枯渇でも `initialized_=true`。
- **L-8** `memory_subsystem.h:16`: `MemoryConfig::use_large_pages` がどこからも参照されず、実際の有効化はコンパイル時マクロのみ。設定 API として虚偽。
- **L-9** `soa_stream.h:27-28`: move ctor/assign が `= default` で move 元の `data_` が null 化されない (aliasing 事故の温床)。

## 良い点

- **PoolAllocator** (`pool_allocator.cpp:43, 97`): 全確保 64B 切り上げ + `aligned_alloc(64)` で `float4x4` (alignas(64) + static_assert) の SoA が常に SIMD 整列を満たす。MSVC `_aligned_malloc/_aligned_free` 対応も正しい。
- **RadixSort** (`radix_sort.cpp:48-58`): ping-pong の奇数パス copy-back が正しく、安定ソート性も保持。`SortPair` 16B の static_assert。
- **GpuMemoryAllocator::free** (`gpu_memory_allocator.cpp:98-122`): 前後隣接ブロックのマージは正しい (正常系のフラグメンテーション対策は健全)。
- **SceneRegistry** (`scene_registry.cpp:39-57`): swap-and-pop の id_map 付替えは正しい。
- **ThreadPoolDispatcher の shutdown** (`job_dispatcher.cpp:20-26, 61-67`): shutdown フラグ → notify_all → join の順序が正しく終了時ハングなし。

**最優先**: C-1 (毎フレーム踏む競合)、H-1 (描画欠落として即顕在化)、H-3 (実 Vulkan 接続時に GPU 破壊データの温床)。
