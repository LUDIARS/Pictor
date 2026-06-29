# GPU — GPU-driven パイプライン + バッファ管理

> 実装: `include/pictor/gpu/` (2), `src/gpu/`

オブジェクトデータを GPU 常駐 SoA バッファ化し、compute シェーダ連鎖で update→cull→LOD→
indirect draw 生成までを on-device で回す GPU-driven レイヤー。

## 構成

| クラス | 役割 |
|---|---|
| `GPUBufferManager` | GPU SoA SSBO / mesh pool / instance / indirect / staging を確保。dirty region 追跡で CPU→GPU 転送最適化 |
| `GPUDrivenPipeline` | GPU-driven の全段を統括: compute update → 2 段 cull (frustum + Hi-Z) → LOD 選択 → compact → indirect draw 生成 |
| `GPUDrivenConfig` | max tri=50000 / min instance=32 / workgroup=256 / two_phase_culling / compute_update |

## 主要 API

```cpp
// GPUBufferManager
SSBOLayout allocate_soa_buffers(uint32_t object_count);
void resize_soa_buffers(SSBOLayout&, uint32_t new_count);
MeshAllocation allocate_mesh(size_t vbytes, size_t ibytes);
void mark_dirty(uint32_t start, uint32_t end, uint32_t chunk=16384);
// GPUDrivenPipeline
void initialize(uint32_t max_objects);
void upload_initial_data(const ObjectPool&);
void execute(const Frustum&, const UpdateScheduler::ComputeUpdateParams&);
Stats get_stats() const;   // {total/visible objects, draw_calls, workgroups}
```

## SoA レイアウト (SSBOLayout)

| バッファ | 中身 | 更新 |
|---|---|---|
| gpu_bounds (24B) / gpu_transforms (64B) / gpu_velocities (12B) | per-object 変動 | compute update |
| gpu_mesh_info / gpu_material_ids / gpu_lod_info | 安定 metadata | upload 時 |
| gpu_visibility (4B) | cull + LOD bits | compute cull 出力 |
| indirect_draw (`DrawIndexedIndirectCommand` 20B) / draw_count (atomic) | compact 出力 | GPU |

## 2 段カリング

`gpu_driven_pool` を SSBO 化 → ① frustum cull で画面内候補に絞る → ② Hi-Z occlusion で遮蔽除外。可視のみ indirect draw entry を生成 ([culling.md](culling.md) の GPU 側に対応)。

## 位置 / 依存

フレームでは **CPU update の後・render pass 記録の前**。compute dispatch は update と graphics の間で非同期実行、出力 (indirect buffer + draw_count) が graphics command 記録へ。依存: `memory/gpu_memory_allocator.h` 経由で VkBuffer (公開 IF に Vulkan ヘッダ非露出)。関連: [batch.md](batch.md) / [update.md](update.md)。
