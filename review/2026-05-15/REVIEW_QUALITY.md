# 品質保証レビュー (Quality Assurance Review) — Pictor v2.1

| 項目 | 値 |
|------|-----|
| リポジトリ | LUDIARS/Pictor |
| 対象ブランチ / PR | main |
| レビュー実施日 | 2026-05-15 |
| 対象コミット範囲 | HEAD~9..HEAD |

---

## 1. テスト戦略・カバレッジ (Test Strategy & Coverage)

| 評価 | 観点 | 所見 |
|------|------|------|
| B | unit テストの網羅性 | frame_allocator, radix_sort, culling 3 個のみ。postprocess, decal, animation 未収 |
| B | integration テストの網羅性 | pixel_text_test, fps_baseline_test。decal ↔ postprocess 統合テストなし |
| B | E2E テストの存在 | graphics_demo, ocean_demo で visual 検証のみ |
| B | エッジケース・境界値テスト | culling test は境界含む、decal OBB boundary 未 |
| A | CI でのテスト自動実行 | GitHub Actions (build.yml): Ubuntu build + CTest |

---

## 2. パフォーマンス・ベンチマーク (Performance & Benchmark)

| 評価 | 観点 | 所見 |
|------|------|------|
| B | パフォーマンス要件の明文化 | README は "high-efficiency" 表現、具体的 SLO なし |
| B | ベンチマーク実装 | pictor_benchmark (1M spheres)、decal/postprocess 性能 benchmark なし |
| B | プロファイリング (CPU / メモリ / I/O) | `PICTOR_ENABLE_PROFILER` あり、export format 文書化不足 |
| B | 性能リグレッション検知 | CI で build のみ、性能回帰自動感知なし |
| C | 大規模データ・高負荷時の挙動 | 100+ decal 性能 unknown |

---

## 3. ライセンス遵守・OSS 帰属表示 (License Compliance)

| 該当依存 | ライセンス | 配布形態 | 互換性評価 | 帰属表示状態 |
|---------|----------|---------|-----------|-------------|
| GLFW 3.4 | zlib | static/dynamic | ✅ OK | NOTICE ファイルなし |
| Vulkan SDK | Apache 2.0 | dynamic | ✅ OK | system-level |
| stb (header-only) | Public Domain / MIT | static | ✅ OK | third_party/stb |
| Rive Renderer (optional) | Proprietary (Rive's license) | static | ⚠️ review needed | `PICTOR_ENABLE_RIVE=ON` 時 |

**評価**: A

---

## 4. クロスプラットフォーム互換 (Cross-Platform Compatibility)

| 評価 | 観点 | 所見 |
|------|------|------|
| A | パス区切り・大文字小文字の扱い | std::filesystem 未使用、std::string ホスト責務 |
| A | プロセス・IPC の OS 別実装 | in-process library、IPC なし |
| B | 文字エンコーディング・改行コード | shader 読込は binary。MSVC `/utf-8` flag 指定 |
| A | ビルドツールチェーンの差分 | CMake 3.20+, conditional (AVX2 ARM64 回避, /W4) |
| C | CI でのマトリクス実行 | Ubuntu only (build.yml matrix なし) |

---

## 5. ドキュメント完備性 (Documentation Completeness)

| 評価 | 観点 | 所見 |
|------|------|------|
| A | README 完備性 | quickstart、API 表、config schema 例 |
| A | DESIGN / アーキテクチャ図 | 設計書・実装段階詳細 |
| A | API リファレンス | header に doc comment、Tauri IPC ドキュメント |
| A | inline コメント粒度 | 詳細日本語注記 |
| A | 開発者向け CONTRIBUTING / ランブック | CLAUDE.md、android-build.md 等 |

---

## 総合評価

| # | レビュー観点 | 評価 | 重大指摘数 |
|---|------------|------|-----------|
| 1 | テスト戦略・カバレッジ | C | 1 |
| 2 | パフォーマンス・ベンチマーク | B | 1 |
| 3 | ライセンス遵守 | A | 0 |
| 4 | クロスプラットフォーム互換 | B | 2 |
| 5 | ドキュメント完備性 | A | 0 |
