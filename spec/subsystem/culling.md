# Culling — 視錐台/空間分割によるカリング

> 実装: `include/pictor/culling/`, `src/culling/`

frustum を入力に可視オブジェクト集合を求める多段カリング。CPU 側 (WorldPartition 広域 → FlatBVH 詳細) と GPU 側 (Hi-Z occlusion) に分かれる。

## CullingSystem (culling_system.h)

オーケストレータ。`SceneRegistry` の各 `ObjectPool` を視錐台でふるい、`uint8_t* visibility_flags` (1=可視/0=cull) を設定する。

```cpp
explicit CullingSystem(SceneRegistry& registry);
void configure_partition(const WorldPartitionConfig&);     // grid 広域分割を有効化
void rebuild_partition(PoolType);                          // static/dynamic の grid 再構築
void build_static_bvh(PoolAllocator&);                     // static は SAH で flat BVH 構築
void refit_dynamic_bvh();                                  // dynamic は bound を bottom-up 更新
void cull(const Frustum&, FrameAllocator&);                // 全段を 1 回で実行
Stats get_stats() const;                                   // total/visible/culled/cells_*/cull_ratio
void set_culling_provider(ICullingProvider*);              // カスタムカリング差替
```

- 入力 `Frustum` は view-projection から `frustum_utils::extract_frustum()` で 6 平面抽出
- 対象プール: `static_pool` (partition+BVH) / `dynamic_pool` (partition+linear) / `gpu_driven_pool` (GPU で cull、CPU flag は全 1)

## FlatBVH (flat_bvh.h)

flat 配列 (DoD) の加速構造。

```cpp
struct alignas(32) BVHNode {           // 32B = 2/cache line
  float3 aabb_min; uint32_t child_or_object_index;   // 内部=左child / leaf=object 開始
  float3 aabb_max; uint32_t flags;                   // bit0=isLeaf, bit1-7=objectCount(≤127)
};
void build(const AABB*, const uint32_t* indices, uint32_t count, PoolAllocator&);  // binned SAH(12bin) + vEB レイアウト
void refit(const AABB*);                                                            // leaf→root bound 更新
bool needs_rebuild(float quality_threshold = 2.0f);
uint32_t query_frustum(const Frustum&, uint32_t* out_visible, uint32_t max) const;  // stack 走査 (64要素)
uint32_t query_aabb(const AABB&, uint32_t* out, uint32_t max) const;
```

- 構築: iterative stack + per-axis binned SAH、leaf ≤4 obj、max node = 2N-1。構築後に BFS 順の **van Emde Boas レイアウト**へ並べ替えて cache 最適化
- refit は構造を保ったまま dynamic 用に bound のみ更新

## WorldPartition (world_partition.h)

一様 3D グリッドの広域分割。

```cpp
struct WorldPartitionConfig { float3 origin; float world_size=10000; float cell_size=100; };
struct PartitionCell { AABB bounds; std::vector<uint32_t> object_indices; };
void configure(const WorldPartitionConfig&);
void rebuild(const AABB*, uint32_t count);
void assign_object(uint32_t index, const AABB&);   // 旧 cell から移動 (incremental)
void remove_object(uint32_t index);
uint32_t query_frustum(const Frustum&, const PartitionCell** out, uint32_t max) const;  // cell の AABB が frustum と交差するものを返す
```

- `CellKey` = 64bit (軸 21bit ずつ、負 index 対応)。cell は object AABB の **中心**で決定
- `cells_` (`unordered_map<CellKey, PartitionCell>`) + `object_cell_map_` (object→cell 逆引き)

## フレーム位置

`pictor_renderer.cpp` のメインループ:

```
DataUpdate (transform/bound 更新)
  → CullingSystem::cull(camera.frustum, frame_alloc)   ← 本サブシステム
      Level0: WorldPartition  frustum × cell AABB (cell 単位スキップ)
      Level1: FlatBVH or linear  frustum × 個別 AABB (iterative 走査)
      Level3: gpu_driven_pool は GPU へ委譲
  → BatchBuilder が visibility_flags を読んで draw batch 構築
  → RenderPassScheduler → GPUDrivenPipeline (Hi-Z compute cull)
profiler に visible/culled を record
```

## GPU カリング

`gpu_driven_pool` は compute で 2 段カリング:

- `shaders/compute_cull.comp` (local_size 256): `gpu_bounds`(SSBO) + frustumPlanes + viewProj + `hiZTexture` → `gpu_visibility`。frustum test → Hi-Z occlusion test
- `shaders/hiz_build.comp` (16×16): max-depth の mip ピラミッド生成
- `GPUDrivenConfig { two_phase_culling, compute_update }`

## 依存

`pictor/core/types.h` (AABB/Frustum/float3)、`scene/scene_registry.h`、`memory/{frame,pool}_allocator.h`、標準ライブラリのみ。CPU 側は外部数学ライブラリ非依存。
