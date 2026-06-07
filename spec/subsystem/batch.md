# Batch — draw batch / indirect コマンド生成

> 実装: `include/pictor/batch/` (2), `src/batch/`

culling 結果 (`visibility_flags`) と shader/material キーから描画バッチを生成する。
データは動かさず index 間接で参照する。

## 構成

| クラス | 役割 |
|---|---|
| `BatchBuilder` | ObjectPool の visibility_flags + shader_keys を読み、SortPair を radix sort して `RenderBatch[]` を生成 (データ移動なし、index 間接) |
| `RadixSort` | 64bit キーの LSB-first radix sort (O(n)、8 pass × 8bit、stable)。temp は FrameAllocator |
| `IBatchPolicy` | 隣接バッチ結合のカスタムポリシー (`should_merge(key_a, key_b)`) |

## sort key

`build_sort_key(render_pass_id:63-60, transparency:59-56, shader_key:55-40, material_key:39-24, depth:23-0)`。cull 済 (visibility=0) は `UINT64_MAX` にして末尾へ送り batch から除外。

## プール別生成

| プール | 方式 |
|---|---|
| STATIC | sort → (shaderKey,materialKey) で連続グループ化 → `RenderBatch[]` (Multi Draw Indirect) |
| DYNAMIC | 上記 + `sorted_indices_[i]=pairs[i].index` を保持 (instanced、per-instance 間接) |
| GPU_DRIVEN | CPU batch なし、プール全体を 1 RenderBatch (compute が cull/LOD/draw 生成) |

## 主要 API

```cpp
explicit BatchBuilder(SceneRegistry&);
void build(FrameAllocator&);                  // 全プール再構築
void invalidate_pool(PoolType);
void set_batch_policy(IBatchPolicy*);
const std::vector<RenderBatch>& batches() const;
const uint32_t* sorted_indices() const;       // SoA への間接 index
Stats get_stats() const;
```

## データフロー

データは動かさない: `RenderBatch.startIndex` が sorted_indices を指し、render pass が sorted_indices 経由で ObjectPool の SoA (transform/material) を間接アクセスする。

## 位置 / 依存

フレームでは **culling の後・render の前**: scene → culling(visibility) → **batch** → render pass。依存: `memory/{pool,frame}_allocator.h`、`scene/`、`core/types.h` (RenderBatch)。関連: [scene.md](scene.md) / [culling.md](culling.md) / [gpu.md](gpu.md)。
