# REVIEW_MISSING_FEATURES.md — Pictor (2026-05-13)

評価: **B**

主要機能は揃うが、運用支援と LUDIARS 共通構成にギャップ。

## 不足

- M1. CI build/test 不在 (中)。`.github/workflows/` は Claude review のみ。`tests/` の 5 CTest を回す GHA を追加すべき。
- M2. CLAUDE.md / DESIGN.md / spec/ 不在 (中)。`plan.md` → `DESIGN.md` リネーム + `CLAUDE.md` 新設 (上位非依存 / Rive 単一情報源 / Debug 既定) を推奨。
- M3. Rive smoke test 不在 (低)。minimal .riv 未同梱で `PICTOR_ENABLE_RIVE=ON` の load→render→shutdown を CI 検証できない。

## 観察

- `docs/android-build.md` は plan 止まり、Android toolchain 分岐未実装。
- `tools/feature-selector` は `PICTOR_BUILD_TOOLS=OFF` 既定で影響軽微。
