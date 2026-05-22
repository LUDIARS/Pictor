# 実装評価 (Implementation Evaluation) — Pictor v2.1

| 項目 | 値 |
|------|-----|
| リポジトリ | LUDIARS/Pictor |
| 対象ブランチ / PR | main |
| レビュー実施日 | 2026-05-15 |
| 対象コミット範囲 | HEAD~9..HEAD |

---

## 1. コード品質 (Code Quality)

| 該当箇所 | 問題分類 | 説明 | 推奨修正 |
|----------|---------|------|---------|
| `src/decal/decal_system.cpp:31-84` | マジックナンバー + 複雑度 | `mat4_inverse()` の逆行列計算が 16 × 16 展開 | small Pictor では inline OK、math util 分離検討 |
| `src/postprocess/postprocess_pipeline.cpp:634` | 条件分岐チェイニング | init phase の sequential `if (!create_..) { return; }` | 現行 style OK (明示性のため) |
| `src/postprocess/postprocess_pipeline.cpp:52-64` | エラー伝播不足 | `find_memory_type_()` 失敗時 UINT32_MAX 返却 | **High**: error code or `std::optional<uint32_t>` 推奨 |
| `include/pictor/decal/decal_system.h:90-110` | ヘッダ伝播 | `#ifdef PICTOR_HAS_VULKAN` 内部に API public | Low: 現行 adequate |

---

## 2. データスキーマの妥当性・重複確認 (Data Schema Validation)

| テーブル / モデル | 問題種別 | 説明 | 推奨対応 |
|-----------------|---------|------|---------|
| `DecalDescriptor` | 正規化完了 | TRS transform [16], texture handle, blend mode, opacity, fade params | OK |
| `PostProcessConfig` (含む HDRConfig/BloomConfig/…) | 正規化完了 | 各 effect 設定が分離された struct、重複なし | OK |
| `DecalEntry` (内部) | 正規化完了 | descriptor + sort_order + vk resource handle | OK |
| `RenderTarget` (内部) | alignment 懸念 | image, imageMemory, view, framebuffer, extent | Low: Vulkan handle は opaque ptr |

**評価**: A

---

## 3. SRE観点のレビュー (SRE Review)

| 評価 | 観点 | 所見 |
|------|------|------|
| B | 可観測性 (Observability) | fprintf(stderr) via init failure、real-time render 中 metric なし |
| A | デプロイ安全性 | static library (libpictor.a)、CMake で feature toggle |
| A | スケーラビリティ | decal 最大 64 個 hardcode、100+ 必要時 clusterization (P3) |
| B | 障害復旧 (Disaster Recovery) | `shutdown()` 存在するが、RAII 弱め |
| B | 依存関係管理 | system Vulkan SDK、GLFW auto-fetch |

---

## 総合評価

| # | レビュー観点 | 評価 | 重大指摘数 |
|---|------------|------|-----------|
| 1 | コード品質 | B | 2 |
| 2 | データスキーマ | A | 0 |
| 3 | SRE | B | 1 |
