# subsystem — 基盤サブシステム設計

Pictor (低レイヤ Vulkan 描画ライブラリ) の基盤サブシステムごとの設計資料。
パイプライン上位の設計は `spec/rendering-extensibility-design.md` /
`spec/pipeline-*.md` を参照。

## フレームパイプライン順

```
data(asset) → scene(SoA store) → update(transform) → culling → batch → gpu(driven)
            → render passes [ gi(shadow/AO/probe) → decal → post-process ]
surface / memory / shader / text は横断基盤、webgl は Vulkan と並行の代替バックエンド
```

## 一覧

| ファイル | サブシステム | 概要 |
|---|---|---|
| `surface.md` | surface | プラットフォーム抽象 (`ISurfaceProvider`) + `VulkanContext` (instance/device/swapchain) |
| `memory.md` | memory | frame(bump) / pool(SoA) / GPU サブアロケータ。DoD を支える |
| `data.md` | data | texture/mesh/model のアセットライフサイクル (FBX 等) |
| `scene.md` | scene | `SceneRegistry` + `ObjectPool` の SoA オブジェクト store |
| `update.md` | update | per-frame transform 更新スケジューラ (CPU並列/NT/GPU compute 自動選択) |
| `culling.md` | culling | WorldPartition 広域 → FlatBVH 詳細 → GPU Hi-Z の多段カリング |
| `batch.md` | batch | `BatchBuilder` + radix sort で draw batch / indirect 生成 |
| `gpu.md` | gpu | GPU-driven パイプライン (compute update→cull→LOD→indirect) + バッファ管理 |
| `shader.md` | shader | Visus カスタムシェーダ登録 + Vulkan pipeline 生成 (SPIR-V 事前コンパイル前提) |
| `gi.md` | gi | 影 (CSM) / AO (SSAO) / 間接光 (irradiance probe) のランタイム + bake |
| `decal.md` | decal | 投影デカール (depth から world 復元 + OBB 投影) |
| `text.md` | text | 自前 TTF/OTF パース + 3 描画経路 (atlas / image / SVG) |
| `webgl.md` | webgl | WebGL2 (Emscripten) 代替バックエンド。Vulkan と並行・非抽象 |

> 各文書は実装 (`include/pictor/<name>/` + `src/<name>/`) を読んで起こしたもの。
> 上位レイヤ (`material` / `postprocess` / `animation` / `vector` / `visus` / `c_api` /
> `profiler` / `ui`) は pipeline 設計書側または別途で扱う。
