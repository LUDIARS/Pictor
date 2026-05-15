# Pictor レビューサマリー (2026-05-13)

- 対象: E:/Document/Ars/Pictor (HEAD = 23529e6, 272 ファイル)
- 役割: LUDIARS 下層描画ライブラリ (Vulkan + 任意 Rive Renderer + WebGL2)。上位ライブラリに依存しない。
- レビュー観点: 設計 / 脆弱性 / 実装 / 不足機能 / 品質

## 総合評価

| 観点 | 評価 |
|------|------|
| Design (設計) | **A-** |
| Vulnerability (脆弱性) | **B+** |
| Implementation (実装) | **B+** |
| Missing Features (不足) | **B** |
| Quality (品質) | **B+** |

**Weighted score: 84 / 100**  (Design 0.25 / Vulnerability 0.20 / Implementation 0.25 / Missing 0.15 / Quality 0.15)

## 主要所見

- 「Pictor は上位ライブラリに依存しない」原則は遵守されている (`memoria` / `actio` / `ergo` / `ergo_custos` / `adventurecube` などへの参照 0、検出語は無関係なコメントのみ)。`demo/rive/main.cpp` も Pictor + GLFW + Vulkan のみで構成され、上位レイヤへの依存はない。
- Rive Renderer 統合 (`PICTOR_ENABLE_RIVE` / `PICTOR_RIVE_DIR`) は `cmake/FindRive.cmake:32-153` と `src/vector/rive_renderer.cpp:1-396` で完結。PICTOR_HAS_RIVE 未定義時は安全な no-op stub になり、下流が無条件に依存できる設計は良好 (`include/pictor/vector/rive_renderer.h:27-34`)。
- 一方で `src/animation/rive_animation.cpp:7-176` は旧プレースホルダ実装が残存し、`RiveRenderer` (新) と `RiveAnimation` (旧 stub) が混在。後者は実際に Rive ランタイムを使っていないのに公開 API として残っており、利用者を誤誘導するリスク。
- 脆弱性は致命的なものなし。ファイル I/O (`load_riv_file` / FBX) でサイズ上限・パストラバーサルチェック未実装、外部 .riv の信頼境界が未定義。
- 不足: CI ビルドワークフロー (`.github/workflows/` は Claude review のみで、build/test を回す GHA がない)。`CLAUDE.md` / `DESIGN.md` が無く `plan.md` が事実上の DESIGN を兼ねる。

## 件数

- 指摘合計: 19 件
  - Design: 4
  - Vulnerability: 4
  - Implementation: 5
  - Missing: 3
  - Quality: 3
- AUTOFIX 自動修正候補: 0 件 (本レビューでは列挙のみ)

## 出力ファイル

- REVIEW.md (本ファイル)
- REVIEW_DESIGN.md
- REVIEW_VULNERABILITY.md
- REVIEW_IMPLEMENTATION.md
- REVIEW_MISSING_FEATURES.md
- REVIEW_QUALITY.md
- AUTOFIX.md
- latest.json
