# AI Code Review — Pictor v2.1

| 項目 | 値 |
|------|-----|
| リポジトリ | LUDIARS/Pictor |
| 対象ブランチ / PR | main (HEAD: 0699061) |
| レビュー実施日 | 2026-05-17 |
| 対象コミット範囲 | HEAD~2..HEAD (3 commits since 2026-05-15) |
| 総変更行数 | 1,431 insertions / 0 deletions (13 files changed) |

---

## 総合評価 (Overall Assessment)

| # | レビュー観点 | 評価 | 重大指摘数 | ドキュメント |
|---|------------|------|-----------|------------|
| 1 | 脆弱性 | A | 0 | [脆弱性レビュー](REVIEW_VULNERABILITY.md) |
| 2 | 設計強度 | A | 0 | [設計レビュー](REVIEW_DESIGN.md) |
| 3 | 設計思想の一貫性 | A | 0 | [設計レビュー](REVIEW_DESIGN.md) |
| 4 | モジュール分割度 | A | 0 | [設計レビュー](REVIEW_DESIGN.md) |
| 5 | コード品質 | B | 1 | [実装評価](REVIEW_IMPLEMENTATION.md) |
| 6 | データスキーマ | A | 0 | [実装評価](REVIEW_IMPLEMENTATION.md) |
| 9 | SRE | A | 0 | [実装評価](REVIEW_IMPLEMENTATION.md) |
| 10 | ゼロトラスト | N/A | - | [脆弱性レビュー](REVIEW_VULNERABILITY.md) |
| 11 | セキュリティ | A | 0 | [脆弱性レビュー](REVIEW_VULNERABILITY.md) |
| 12 | テスト戦略・カバレッジ | B | 1 | [品質保証レビュー](REVIEW_QUALITY.md) |
| 13 | パフォーマンス・ベンチマーク | B | 0 | [品質保証レビュー](REVIEW_QUALITY.md) |
| 14 | ライセンス遵守 | A | 0 | [品質保証レビュー](REVIEW_QUALITY.md) |
| 15 | クロスプラットフォーム互換 | A | 0 | [品質保証レビュー](REVIEW_QUALITY.md) |
| 16 | ドキュメント完備性 | A | 0 | [品質保証レビュー](REVIEW_QUALITY.md) |

---

## 重大指摘サマリー

**Critical / High:**
- なし (新規 Visus レイヤーは安全設計)

**Medium (改善推奨):**
- `src/visus/visus_serializer.cpp:106-116` — handle_from_string() の INVALID_HANDLE_U32 マジックナンバーが unbound. uint64 累算 overflow ガードなし (NULL pointer dereference 軽微危険)
- `tests/unit_visus_serializer_test.cpp` — Visus JSON の edge case テスト (環境変数 token, リモート URL 無効化シナリオ) 未収

---

## 総合所見

**強み:**
- **Visus レイヤー設計は優秀**: Material より高位の「描画定義レシピ」を Material と独立させ, ObjectDescriptor instantiation を委任する設計は拡張性・再利用性に優れる
- **7-kind geometry abstraction**: PRIMITIVE / MODEL / RIVE / UI / PARTICLE / TEXT / CUSTOM を enum で整理, 各種 handle (mesh / model / shader / generic) を kind に応じて切替える構造は堅牢
- **ResourceRef + fetch policy の分離**: リソース取得戦略 (LOCAL_ONLY / CACHE_FIRST / REVALIDATE / ALWAYS_FETCH) を serializable にしたことで, Ergo plugin 側での柔軟な CDN 統合が可能
- **外部依存ゼロの JSON serializer**: 手書きパーサで libcurl 等の HTTP を持たない設計. 上位非依存ルール (memory: pictor_no_upper_dep) を遵守
- **テストカバレッジ充実**: round-trip serializer テスト (190 行) + instantiator テスト (83 行), both PASS
- **docs 同期**: README / DESIGN / plan を最新化 (ce00777, 42d3396)

**改善点:**
- instantiator の material slot 展開ロジック (`instantiate_visus()`) が 1 描画対象あたり 1 ObjectDescriptor として slot 数分 register するため, 「1 visus → N instances」を予定する場合, slot ごとに異なる transform を指定できない (設計的には OK, ドキュメントに明記推奨)
- Visus の shader_key_override (include/pictor/visus/visus.h:167) が uint64_t で, 実際の ObjectDescriptor::shaderKey も uint64_t だが, override を「常に適用」か「slot ごと適用」か「fallback」か不明記
- ResourceRef の header 環境変数展開 (`"${env:CDN_TOKEN}"`) はパース後, 呼び出し側 (Ergo plugin) で実施する設計. Pictor 内には展開コードなし (正しい)

**リスク:**
- Visus instantiation が「base descriptor → slot loop → register」なため, 同一 Visus の複数 instance が異なる transform を持つ場合, SceneRegistry に複数 register されることになる. このとき `update_transform()` で全 instance を同期する責務が呼び出し側にある (ドキュメント明記推奨)

---

## 次ステップ

1. **即時** (current sprint):
   - Visus + Ergo plugin integration test (VisusDesc JSON load → MaterialRegistry 登録 → instantiate → render)
   - ResourceRef 環境変数展開の呼び出し側実装確認 (Ergo 側)

2. **1 sprint**:
   - Visus instantiation の transform 複製シナリオのドキュメント化
   - shader_key_override の詳細動作仕様化

3. **Backlog**:
   - Decal 統合テスト (前回指摘の継続)
   - VisusRegistry 複数登録時の lifecycle 管理
