# 実装評価 (Implementation Evaluation) — Pictor v2.1 (Visus レイヤー)

| 項目 | 値 |
|------|-----|
| リポジトリ | LUDIARS/Pictor |
| 対象ブランチ / PR | main (0699061) |
| レビュー実施日 | 2026-05-17 |
| 対象コミット範囲 | HEAD~2..HEAD |

---

## 1. コード品質 (Code Quality)

| 該当箇所 | 問題分類 | 説明 | 推奨修正 |
|----------|---------|------|---------|
| `src/visus/visus_serializer.cpp:106-116` | handle_from_string() のエラー処理 | 数値変換ループ内で overflow チェックなし. 999...99 入力で INVALID_HANDLE_U32 返却は OK だが, 中途半端な値は拾える | `std::strtoul()` に切替えるか, explicit range check (`v > UINT32_MAX`) 追加. Low 優先度 |
| `include/pictor/visus/visus.h:167` | shader_key_override の用途不明 | CUSTOM kind 専用コメント欠落. decal system との関連も unclear | インラインコメント「// CUSTOM / decal kind でのみ使用. その他は 0」を追加 |
| `src/visus/visus_instantiator.cpp:21-30` | transform / bounds の複製漏れ | slot 数分 ObjectDescriptor を register するが, 同一 visus の複数 instance で異なる transform を使う際, 複製・管理方法を呼び出し側に完全委任 | ドキュメント「各 slot → 別 ObjectId → update_transform(id, transform) で同期」を明記推奨 |
| `tests/unit_visus_serializer_test.cpp:22` | env token hardcode テスト | "${env:CDN_TOKEN}" を mock しない. env 非存在時 parser は成功 (expand は呼び出し側) | 既存テストで十分 (env expand は Pictor 外). Low 優先度 |

---

## 2. データスキーマの妥当性・重複確認 (Data Schema Validation)

| テーブル / モデル | 問題種別 | 説明 | 推奨対応 |
|-----------------|---------|------|---------|
| `VisusDesc` | 正規化完了 | 7 kind に応じた handle + asset + slot の分離. 重複なし | OK |
| `ResourceRef` | 正規化完了 | local / remote / fetch_policy / headers の独立管理 | OK |
| `VisusMaterialSlot` | 正規化完了 | slot_name + material handle + material_resource. 各 slot の独立性確保 | OK |
| `VisusAnimationDefault` | 正規化完了 | kind (clip/SM/rive) + name + loop + speed | OK |

**評価**: A

---

## 3. SRE 観点のレビュー (SRE Review)

| 評価 | 観点 | 所見 |
|------|------|------|
| B | 可観測性 (Observability) | from_visus_json error string は short message のみ. JSON parse failure 時に行番号提供なし |
| A | デプロイ安全性 | static lib に組込. feature toggle (PICTOR_BUILD_TESTS) で headless build 可 |
| A | スケーラビリティ | JSON serializer は streaming 非対応だが, 単一 visus は通常数 KB. 複数 visus は呼び出し側で load |
| A | 障害復旧 | VisusDesc load fail は caller がハンドル. registry unregister で cleanup 可 |
| A | 依存関係管理 | 外部依存なし (HTTP / JSON lib ともホスト注入) |

---

## 総合評価

| # | レビュー観点 | 評価 | 重大指摘数 |
|---|------------|------|-----------|
| 1 | コード品質 | B | 1 |
| 2 | データスキーマ | A | 0 |
| 3 | SRE | A | 0 |
