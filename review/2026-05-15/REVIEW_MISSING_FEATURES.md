# 不足機能評価 (Missing Features) — Pictor v2.1

| 項目 | 値 |
|------|-----|
| リポジトリ | LUDIARS/Pictor |
| 対象ブランチ / PR | main |
| レビュー実施日 | 2026-05-15 |
| 対象コミット範囲 | HEAD~9..HEAD |

---

## 不足機能・改善案

| 優先度 | 機能 | 現状 | 効果 | 推奨スケジュール |
|--------|------|------|------|----------------|
| **High** | Android NDK (Phase 1) | 設計文書完了、コード未実装 | arm64-v8a ビルド成功 | 1 sprint |
| **High** | DecalSystem 統合テスト | unit test なし | decal ↔ postprocess 相互作用検証 | 1 sprint |
| **Medium** | Structured logging | fprintf(stderr) | 性能分析、障害対応効率化 | 2 sprint |
| **Medium** | Decal クラスタ化 (P3) | hardcode max 64 | 100+ decal 描画 | 2 sprint |
| **Medium** | SAST 統合 (CI) | build.yml のみ | code scan for potential bugs | 1 sprint |
| **Low** | Math utility lib 分離 | inline mat4 計算 | コード再利用、保守性 | backlog |
| **Low** | Performance ベンチマーク doc | profiler あるが未文書化 | 性能基準線設定 | backlog |

---

## 優先度マトリックス

|  | コスト 低 | コスト 中 | コスト 高 |
|---|--------|--------|--------|
| **インパクト 高** | SAST (Medium) | Logging (Medium) | Android P1 (High) |
| **インパクト 中** | Math util (Low) | Decal cluster (Medium) |  |
| **インパクト 低** | Perf doc (Low) |  |  |

**即時着手推奨:**
1. Android NDK Phase 1 (1 sprint)
2. Decal integration test (1 sprint)
3. SAST CI (2-3 days)
