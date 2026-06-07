# Data — アセット (texture/mesh/model) ライフサイクル

> 実装: `include/pictor/data/` (6), `src/data/`

GPU リソース (テクスチャ / メッシュ / 3D モデル) の登録・アップロード・破棄を束ねる
ファサード層。全描画 (culling/batch/draw) の上流に位置する。

## 構成

| クラス | 役割 |
|---|---|
| `DataHandler` | texture/vertex/model を束ねる中央ファサード。下記3つへ委譲 |
| `TextureRegistry` | GPU テクスチャの登録/upload/破棄、format-aware サイズ計算、名前引き、swap-and-pop 削除 |
| `VertexDataUploader` | 任意 `VertexLayout` のメッシュ VB/IB 管理、`update_vertex_region()` で部分更新、flat 配列 |
| `ModelDataHandler` | FBX/OBJ/MMD/glTF 等の 3D モデル。SkinMesh(weights/morph) + Rig(skeleton/IK) + AnimationClip を統括、VB は VertexDataUploader へ・skeleton/clip は AnimationSystem へ委譲 |
| `DataQueryAPI` | 読み取り専用の introspection (texture/mesh 列挙、format/semantic filter、JSON エクスポート)。level editor / asset browser 用 |

## 主要 API (抜粋)

```cpp
// TextureRegistry
TextureHandle register_texture(const TextureDescriptor&);
bool upload_texture_data(TextureHandle, const void* data, size_t size, uint32_t mip=0, uint32_t layer=0);
TextureHandle find_by_name(const std::string&) const;
// VertexDataUploader
MeshHandle register_mesh(const MeshDataDescriptor&);
bool upload_vertex_data(MeshHandle, const void* data, size_t size);
bool update_vertex_region(MeshHandle, size_t offset, const void* data, size_t size);
// ModelDataHandler
ModelHandle load_model_from_fbx(const std::string& path);
ModelHandle load_model_from_fbx_memory(const uint8_t*, size_t, const std::string& name);
std::shared_ptr<FBXScene> get_fbx_scene(ModelHandle) const;
```

## レイアウト

- 各レジストリは flat `std::vector<Entry>` + `unordered_map<name, Handle>` の名前引き、削除は swap-and-pop
- テクスチャサイズは `compute_texture_size()` (format-aware: 1-16 B/px、block-compressed 8-16 B/4x4)
- メッシュは `GPUBufferManager::MeshAllocation` (VB/IB = GpuAllocation + count)
- オブジェクトの SoA SSBO は **scene の ObjectPool** 側が持つ (本層は asset 実体を管理)

## 依存 / 位置

`memory/gpu_memory_allocator.h`、`gpu/gpu_buffer_manager.h`、`animation/{animation_system,fbx_importer,fbx_scene}.h`、`core/types.h`。フレームでは **全描画の上流** (アセットロード ↔ フレーム実行の結節点)。関連: [text.md](text.md) (ImageBuffer→texture)、[scene.md](scene.md) (object 登録)。
