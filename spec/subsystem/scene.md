# Scene — オブジェクト store (SoA / ObjectPool)

> 実装: `include/pictor/scene/` (4), `src/scene/`

全描画オブジェクトの SoA ストア。culling/update/batch/render の全下流が参照する土台。

## 構成

| クラス | 役割 |
|---|---|
| `SceneRegistry` | 3 つの `ObjectPool` (STATIC/DYNAMIC/GPU_DRIVEN) を統括。`ObjectId → (PoolType, index)` 逆引き、登録/削除/transform更新/プール移動 |
| `ObjectPool` | 1 カテゴリの SoA。11 個の並列 `SoAStream` を同一 index で保持 |
| `ObjectClassifier` | 登録時に PoolType を決定 (`classify(ObjectDescriptor)`)。GPU 適格判定 (≤50K tri / ≥32 instance / indirect 対応) |
| `SoAStream<T>` | 1 属性の連続配列。`push_back`→index / `swap_and_pop(index)`。cache-line アライン、PoolAllocator が確保 |

## SoA グループ (hot/cold)

ObjectPool は 11 stream を 4 グループに分けてアクセス局所性を出す:

- **A culling(hot)**: `bounds_`(AABB) / `visibility_flags_`(u8)
- **B sort/batch(hot)**: `shader_keys_`(u64) / `sort_keys_`(u64) / `material_keys_`(u32)
- **C transform(dynamic hot)**: `transforms_`(float4x4) / `prev_transforms_`(motion vector 用)
- **D metadata(cold)**: `mesh_handles_` / `material_handles_` / `lod_levels_` / `flags_` / `last_frame_updated_`
- ID: `object_ids_`

## 主要 API

```cpp
ObjectId register_object(const ObjectDescriptor&);   // Classifier がプール決定
void unregister_object(ObjectId);                    // swap_and_pop で密詰め維持
void update_transform(ObjectId, const float4x4&);
void update_bounds(ObjectId, const AABB&);
void change_pool(ObjectId, PoolType);                // プール間移動
ObjectLocation find_object(ObjectId) const;          // {PoolType, index, valid}
void for_each_pool(std::function<void(ObjectPool&, PoolType)>);
```

## ライフサイクル

登録 = Classifier→pool 決定→全 11 stream に同 index で push。削除 = `swap_and_pop` を全 stream に適用し id_map を更新 (末尾と入替)。移動 = 旧プールから状態回収→新プールへ追加。

## 位置 / 依存

フレームでは **最上流のストア**: scene → (update が transform 更新) → culling が `visibility_flags` 書込 → batch が読込。依存: `memory/pool_allocator.h` (SoA 実体)、`core/types.h`。関連: [culling.md](culling.md) / [batch.md](batch.md) / [update.md](update.md)。
