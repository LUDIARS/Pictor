# AUTOFIX.md

## 概要
- 修正ファイル数: 0
- 変更行数: +0 / -0
- カテゴリ別件数: lint=0 / typo=0 / unused_import=0 / dead_code=0 / gitignore=0 / toc=0
- 関連 PR: なし

## 修正対象なし

旧 frame work (bloom_effect.cpp 等) は git diff 確認の上完全削除済。残置のヘッダ shim も post-process refactor 設計 §3.1 で scheduled removal。本日のレビュー範囲では自動修正可能な確実指摘なし。

## フラグしたが手作業に回した指摘 (= 自動修正の範囲外)

- `src/postprocess/postprocess_pipeline.cpp:52-64`, `src/decal/decal_system.cpp:95` — `find_memory_type_()` 失敗時の `UINT32_MAX` 返却を `std::optional<uint32_t>` 等に変更。挙動変更を伴う (REVIEW_IMPLEMENTATION.md §1)
- tests/CMakeLists.txt — `DecalSystem` / `PostProcessPipeline` 統合テスト追加 (REVIEW_QUALITY.md §1)
- .github/workflows/build.yml — CI matrix (Windows/macOS/Linux/Android) 追加 (REVIEW_QUALITY.md §4)
- Structured logging migration (`fprintf(stderr)` → JSON log) — REVIEW_IMPLEMENTATION.md §3 / REVIEW_MISSING_FEATURES.md
- README.md TOC 追加 — 記述スタイル変更のため auto-fix 対象外
- `*.pdb` を .gitignore に追加 — Debug/Release dirs で既に ignored、影響軽微

## 関連
- レビュー全文: REVIEW.md / REVIEW_*.md
- 修正 PR diff: なし
