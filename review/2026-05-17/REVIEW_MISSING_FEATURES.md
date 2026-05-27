# 不足機能評価 (Missing Features) — Pictor v2.1 (Visus レイヤー)

| 項目 | 値 |
|------|-----|
| リポジトリ | LUDIARS/Pictor |
| 対象ブランチ / PR | main (0699061) |
| レビュー実施日 | 2026-05-17 |
| 対象コミット範囲 | HEAD~2..HEAD |

---

## 不足機能・改善案

| 優先度 | 機能 | 現状 | 効果 | 推奨スケジュール |
|--------|------|------|------|----------------|
| **High** | Visus + Ergo plugin integration test | Pictor unit test のみ (no E2E) | visus.json load → instantiate → render 検証 | 1 sprint |
| **Medium** | VisusDesc update_transform sync helper | 呼び出し側で複数 ObjectId を管理 | slot → id mapping utility | 1 sprint |
| **Medium** | ResourceRef validation (caller side) | env expand / remote fetch は Pictor 外 | Ergo plugin 内で implement | 1 sprint |
| **Low** | JSON parse error 行番号報告 | error string は short message のみ | debugging 効率化 | backlog |
| **Low** | VisusAnimationDefault の validation | kind と name の整合性チェックなし | invalid animation ref 検出 | backlog |

---

## 優先度マトリックス

|  | コスト 低 | コスト 中 | コスト 高 |
|---|--------|--------|--------|
| **インパクト 高** | error 行番号 (Low) | Ergo integration (High) | |
| **インパクト 中** | animation validation (Low) | update sync helper (Medium) |  |
| **インパクト 低** |  |  |  |

**即時着手推奨:**
1. Visus ↔ Ergo plugin integration test (1 sprint)
2. update_transform() helper function (3-5 days)
