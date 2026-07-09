# AI Code Review — Pictor v2.1 (Fable 診断: 設計 + 実装ギャップ + 自動修正)

| 項目 | 値 |
|------|-----|
| リポジトリ | LUDIARS/Pictor |
| 対象ブランチ / PR | main (HEAD: 8318501) → fix branch `claude/design-review-gaps-bh10gt` |
| レビュー実施日 | 2026-07-09 |
| 対象範囲 | (1) 前回全量診断 (2026-06-11) の全 38 指摘の残存検証、(2) 前回以降 23 コミット (+10,484 行) の新規レビュー、(3) 自動修正の適用 |
| レビュー方式 | 3 並列検証 (設計指摘追跡 / 実装指摘追跡 + autofix 可否判定 / 新規差分レビュー)。全指摘は実コード読解で file:line 検証済み |

---

## 総合所見

**前回からの改善が実際に入った回。** 2026-06-11 診断の Critical 1 件 (lost wakeup) と High 帯の大半 (God Class 分割 / 虚偽統計 / free ミスマッチ / string 確保) は #80 / #97 で修正済みだった。今回はその検証に加え、残存していた「小さく直せるが実害のある」バグ 12 件と、新規コードの High 1 件 (GIBakeSystem UAF) を含む 13 件を自動修正した。

### 自動修正 (25 項目 / 17 ファイル + テスト 1 追加)

| 分類 | 件数 | 代表例 |
|---|---|---|
| メモリ安全 / UAF | 5 | GIBakeSystem ダングリング参照、SoAStream move、swap_and_pop アンダーフロー、stale sorted_indices、upload null memcpy |
| 描画正しさ | 4 | **static 最終バッチのキャッシュ漏れ (2 フレーム目消失)**、mark_dirty 範囲漏れ (GPU stale データ)、RadixSort 枯渇時フォールバック、viewport 未設定 draw |
| 並行性 / Vulkan 契約 | 3 | peak_ データレース、descriptor pool 破棄前の idle 待ち、recompile 失敗時の disengage |
| 堅牢性 / DoS | 4 | DockLayout パーサ (OOM + 無限再帰)、SPIR-V 検証、SSBO 確保失敗検出、aligned_alloc 規格外 |
| 状態機械 / 統計 | 5 | auto-downgrade sentinel、clear_dirty カウンタ、get_stats 二重加算、refit コスト未更新、shutdown 順序 |
| DoD (per-frame alloc) | 3 | snippet 全文コピー + substr、chrome vector、(bake 再生成は UAF 分類に含む) |
| 死コード | 1 | 未使用 `<unordered_map>` include |

検証: ヘッドレス構成 + Vulkan/demo 構成の両ビルド、ctest **19/19 パス** (回帰テスト 1 追加)。

### 未修正 (設計作業が必要な持ち越し)

- **N-M2**: 既定 recorder が OPAQUE/TRANSPARENT 両パスで全バッチ描画 (透明フラグの運搬設計が必要)
- **H-3 残り**: SSBO free-list の即時解放 (fence ベース遅延回収)
- M-9/M-10 (BVH allocator / スタック飽和)、M-11 (WorldPartition index 衝突)、M-4 (二重 free 防御)
- 設計系: M-3 (framebuffer 位置依存)、M-4 (umbrella ヘッダ、露出 16→22 に拡大中)、M-1 (WorldPartition map)、D-3 残り (GpuTimer strcmp)

詳細: [設計レビュー](REVIEW_DESIGN.md) / [実装ギャップ評価](REVIEW_IMPLEMENTATION.md)
