# 設計レビュー (Design Review) — Pictor v2.1 (Visus レイヤー追加)

| 項目 | 値 |
|------|-----|
| リポジトリ | LUDIARS/Pictor |
| 対象ブランチ / PR | main (0699061) |
| レビュー実施日 | 2026-05-17 |
| 対象コミット範囲 | HEAD~2..HEAD |

---

## 1. 設計強度 (Design Robustness)

| 評価 | 観点 | 所見 |
|------|------|------|
| A | 障害分離 | VisusDesc ↔ ObjectDescriptor 層を明確に分離. Visus 読込失敗時も Material / Texture registry に影響なし |
| A | 冪等性 | from_visus_json() は純粋関数 (side-effect なし). 複数呼び出しで同じ VisusDesc を得る |
| A | 入力バリデーション | JSON parse エラー / handle 形式 ("handle:123" vs "none") の厳密チェック. 未知 key は silent skip |
| A | エラーハンドリング | from_visus_json() は error 文字列ポインタで短い理由を返却. 呼び出し側で判定可能 |
| A | リトライ・タイムアウト設計 | ResourceRef fetch は IResourceLoader 委任. Pictor は timeout 持たない (ホスト責務) |
| A | 状態管理の明確性 | ResourceRef::FetchPolicy で取得戦略を明示. resource_loader は local/remote 決定権を持つ |

---

## 2. 設計思想の一貫性 (Design Philosophy Compliance)

| 該当箇所 | 逸脱内容 | 本来の設計思想 | 推奨修正 |
|----------|---------|--------------|---------|
| `include/pictor/visus/visus.h:147-149` | shader_key_override が CUSTOM kind 専用だが, 他 kind での override 可 | 設計初期は PRIMITIVE / MODEL でも override を想定していた | ドキュメント明記:「CUSTOM / decal 用. 他 kind では 0 を推奨」 |
| `src/visus/visus_instantiator.cpp:21-30` | slot 数分の ObjectDescriptor を別々 register | Material slot を単一責務オブジェクトとして扱う | 現行設計は正当. update_transform() で複数 ID を同期する呼び出し側責務を明記 |
| `include/pictor/visus/resource_loader.h:32-37` | FileSystemResourceLoader は env expand しない | env template は呼び出し側 (Ergo plugin) で展開 | 現行正しい (上位非依存). Pictor は local_path のみ処理 |

---

## 3. モジュール分割度 / 機能的凝集度 (Cohesion & Modularity)

| モジュール / クラス | 凝集度評価 | 所見 |
|-------------------|-----------|------|
| `VisusDesc` struct | 機能的 (strong) | 「geometry kind + asset + slot + animation default」. Material より上位の責務を 1 概念で表現 |
| `VisusMaterialSlot` / `VisusTextureSlot` | 機能的 (strong) | slot_name + handle + resource. descriptor 内 slot 配置用 |
| `ResourceRef` struct | 機能的 (strong) | local + remote + fetch_policy + headers. 取得戦略の完全な表現 |
| `VisusSerializer` (手書きパーサ) | 機能的 (strong) | JSON ↔ VisusDesc のみに責務を限定. enum/handle/float 変換ヘルパも内含 |
| `FileSystemResourceLoader` | 機能的 (strong) | local_path のみ処理. remote は呼び出し側ローダで委任 |
| `instantiate_visus()` function | 機能的 (strong) | base descriptor 構築 → slot loop → register. 単一責務 |

**総合評価:** モジュール分割度 **A**. 各クラスが単一責務に集中し, Material / Texture registry 等への依存が最小限.

---

## 総合評価

| # | レビュー観点 | 評価 | 重大指摘数 |
|---|------------|------|-----------|
| 1 | 設計強度 | A | 0 |
| 2 | 設計思想の一貫性 | A | 0 |
| 3 | モジュール分割度 | A | 0 |
