# 設計レビュー (Design Review) — Pictor v2.1

| 項目 | 値 |
|------|-----|
| リポジトリ | LUDIARS/Pictor |
| 対象ブランチ / PR | main |
| レビュー実施日 | 2026-05-15 |
| 対象コミット範囲 | HEAD~9..HEAD |

---

## 1. 設計強度 (Design Robustness)

| 評価 | 観点 | 所見 |
|------|------|------|
| A | 障害分離 | シーン描画・ポストプロセス・デカール が render pass で明確に分離 |
| A | 冪等性 | `initialize_vulkan()` → `shutdown()` → `initialize()` の往復が安全 |
| B | 入力バリデーション | HDR フォーマット対応確認済み、`DecalSystem::add()` の range check 欠落 |
| A | エラーハンドリング | 各初期化段階で bool / VkResult 確認、失敗時は stderr 出力 |
| B | リトライ・タイムアウト設計 | Vulkan コマンドのタイムアウトはホスト責務 |
| A | 状態管理の明確性 | `is_initialized()` で初期化済みを確認可能 |

---

## 2. 設計思想の一貫性 (Design Philosophy Compliance)

| 該当箇所 | 逸脱内容 | 本来の設計思想 | 推奨修正 |
|----------|---------|--------------|---------|
| `src/postprocess/postprocess_pipeline.cpp:61-63` | HDR フォーマット失敗を silent IDENTITY に | フォーマット検証は initialize_vulkan 時に全実施 | 現行で OK |
| `src/decal/decal_system.cpp:95` | `find_memory_type_()` 失敗時 `UINT32_MAX` 返却 | エラーは bool / nullptr で伝播 | `std::optional<uint32_t>` or explicit error check 推奨 |
| `include/pictor/decal/decal_system.h` | Decal 最大数が `constexpr uint32_t kMaxDecals = 64` | 設定ファイル / runtime 拡張性 | P3 対応 |

---

## 3. モジュール分割度 / 機能的凝集度 (Cohesion & Modularity)

| モジュール / クラス | 凝集度評価 | 所見 |
|-------------------|-----------|------|
| `PostProcessPipeline` | 機能的 (strong) | HDR render pass / bloom extract / blur / color grade の 4 fullscreen pass が 1 概念で統一 |
| `DecalSystem` | 機能的 (strong) | デカール投影・OBB 判定・深度復元・UV サンプリングが 1 責務に凝縮 |
| `VulkanContext` | 通信的 (medium) | Vulkan device/queue/instance の統合 wrapper |
| `AnimationSystem` | 手続き的 (weak-medium) | animation_clip / skeleton / ik_solver / montage を結合。分割候補 (P2 対応) |

---

## 総合評価

| # | レビュー観点 | 評価 | 重大指摘数 |
|---|------------|------|-----------|
| 1 | 設計強度 | A | 0 |
| 2 | 設計思想の一貫性 | A | 0 |
| 3 | モジュール分割度 | A | 0 |
