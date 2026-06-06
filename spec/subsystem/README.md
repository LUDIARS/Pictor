# subsystem — 基盤サブシステム設計

Pictor (低レイヤ Vulkan 描画ライブラリ) の基盤サブシステムごとの設計資料。
パイプライン上位の設計は `spec/rendering-extensibility-design.md` /
`spec/pipeline-*.md` を参照。

| ファイル | サブシステム | 概要 |
|---|---|---|
| `surface.md` | surface | プラットフォーム抽象 (`ISurfaceProvider`) + `VulkanContext` (instance/device/swapchain) |
| `memory.md` | memory | frame(bump) / pool(SoA) / GPU サブアロケータ。DoD を支える |
| `culling.md` | culling | WorldPartition 広域 → FlatBVH 詳細 → GPU Hi-Z の多段カリング |
| `text.md` | text | 自前 TTF/OTF パース + 3 描画経路 (atlas / image / SVG) |

> 各文書は実装 (`include/pictor/<name>/` + `src/<name>/`) を読んで起こしたもの。
> 未文書のサブシステム (data / scene / batch / gpu / shader / gi / update / decal /
> webgl 等) は順次追加予定。
