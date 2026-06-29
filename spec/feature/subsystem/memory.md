# Memory — フレーム/プール/GPU アロケータ

> 実装: `include/pictor/memory/`, `src/memory/`

DoD 方針 (per-frame の scatter alloc を作らない、flat array、参照データは init 1 回ロード) を
支えるメモリ基盤。永続 (scene 寿命) と一時 (per-frame) を分離し、外部アロケータ (VMA) 非依存。

## MemorySubsystem (memory_subsystem.h)

アロケータ群を束ねる umbrella。

- 所有: `FlightFrameAllocator flight_allocator_` / `PoolAllocator pool_allocator_` / `GpuMemoryAllocator gpu_allocator_`
- API: `frame_allocator()` (= flight の current) / `pool_allocator()` / `gpu_allocator()` / `get_stats()`
- `begin_frame()`: `flight_allocator_.advance_frame()` (次 flight へ回転 + reset) → `gpu_allocator_.reset_ring_buffers()` → ++frame_number。`end_frame()` は no-op (cleanup は次 begin に遅延)
- `MemoryConfig`: frame_allocator_size=16MB / flight_count=3 / pool_chunk_size=64KB / gpu_config / use_large_pages

## FrameAllocator (frame_allocator.h) — 線形 bump

per-frame の使い捨て。O(1)、個別 free 無し、lock-free。

```cpp
void* allocate(size_t size, size_t alignment = 16);   // atomic CAS で bump
template<typename T> T* allocate_array(size_t count);
void  reset();                                        // フレーム頭で巻戻し
size_t used() / peak() / capacity() const;
```

- バッファは 64B アライン (`_aligned_malloc` / `std::aligned_alloc`)、任意で 2MB large page (`VirtualAlloc(MEM_LARGE_PAGES)` / `mmap(MAP_HUGETLB)`)
- `std::atomic<size_t>` の relaxed CAS で多スレッド bump (peak は非 atomic、統計用途で許容)

### FlightFrameAllocator — 三重バッファ

`FrameAllocator` を N 個 (既定 3) 回転して CPU を GPU フレーム遅延から隔離。`current()` / `advance_frame()` (= `(idx+1)%N` + その allocator を reset)。フレーム N は flight (N%3) に書き、GPU が N-2 を処理中でも安全。

## PoolAllocator (pool_allocator.h) — 永続 SoA backing

長寿命の SoA インスタンスデータ用。chunk 単位で必要時に伸長、free 無し (allocate-only)。

```cpp
void* allocate(size_t size);                          // 64B (cache line) アライン
void* allocate_array(size_t element_size, size_t count);
void* reallocate_array(void* old, size_t elem, size_t old_count, size_t new_count); // 新規確保+memcpy (真の free なし)
void  clear();                                        // 全 chunk 巻戻し (level 遷移用)
size_t total_allocated() / chunk_count() const;
```

- 確保: 64B アライン → 既存 chunk を first-fit → 入らねば `max(chunk_size, size)` の chunk 追加
- `Chunk { data, capacity, used }`。watermark 方式で per-allocation free list は持たない (mono-directional 成長)

## GpuMemoryAllocator (gpu_memory_allocator.h) — VkBuffer サブアロケーション

自前サブアロケータ (VMA 非依存)。用途別 4+1 プール + free-list (coalescing 付き)。

| プール | 既定 | 種別 | reset |
|---|---|---|---|
| mesh (VB/IB) | 256MB | 永続 | 明示 `free()` のみ |
| ssbo (SoA) | 128MB | 永続 | 明示 `free()` のみ |
| instance | 64MB | ring | per-frame |
| indirect | 16MB | ring | per-frame |
| staging (CPU→GPU) | 64MB | ring | per-frame |

```cpp
GpuAllocation allocate_mesh(size_t, size_t align=256);      // 永続
GpuAllocation allocate_ssbo(size_t, size_t align=256);      // 永続
GpuAllocation allocate_instance(size_t, size_t align=16);   // ring
GpuAllocation allocate_staging(size_t, size_t align=16);    // ring
void free(const GpuAllocation&);                            // free-list へ戻し + 隣接 merge
void reset_ring_buffers();                                  // instance/staging を 1 ブロックへ
Stats get_stats() const;                                    // 各プールの used/capacity + fragmentation
```

- `GpuAllocation { buffer_id, offset, size, valid }`。`buffer_id` はプール識別子で、実 VkBuffer/VkDeviceMemory の bind は上位の graphics context が担う (この層は offset 管理のみ)
- 確保: free_list を first-fit + アライン → ブロック分割。`free()` は offset 順挿入 + 前後 coalesce

## DoD 整合

- per-frame の `malloc`/`new` を排除 (bump or ring)
- CPU 確保はすべて 64B cache-line アライン、large page 任意
- 永続 (mesh/ssbo) と一時 (instance/staging/indirect) を物理的に分離 → フレーム境界で ring を一括 reset

## 依存

`<atomic>` / `<vector>` 等の標準のみ。Windows は `<windows.h>` + `<malloc.h>`、POSIX は `<sys/mman.h>` (large page)。VMA / 独自 STL は不使用。
