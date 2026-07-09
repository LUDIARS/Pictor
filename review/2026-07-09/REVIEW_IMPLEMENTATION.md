# 実装ギャップ評価 — Pictor (2026-07-09, Fable 診断)

対象: `review/2026-06-11/REVIEW_IMPLEMENTATION.md` 全 24 指摘の現 HEAD (8318501) での残存検証 + 自動修正。
検証はすべて実コード読解 (file:line) で実施。**自動修正はヘッドレスビルド + Vulkan 有効ビルド (demo 含む) + ctest 19 件パスで検証済み。**

## 前回指摘の追跡

| ID | 指摘 | 6/11 → 7/9 の状態 | 今回の autofix |
|---|---|---|---|
| C-1 | ThreadPoolDispatcher lost wakeup | **修正済** (#80 系: worker 側 notify を mutex 下に) | - |
| H-1 | static バッチ 2 フレーム目消失 | 部分修正 — キャッシュ導入済みだが**最終バッチだけ `batches_` に直接 push されキャッシュ漏れ** | ✅ `out.push_back` に修正 + 回帰テスト `unit_batch_builder_test` 追加 |
| H-2 | FrameAllocator move 代入の解放関数ミスマッチ | **修正済** (`release_()` 一本化) | - |
| H-3 | GPU ring の in-flight 保護なし | 部分修正 — ring はフライト多重化済み。**SSBO free-list の即時解放と「Fence wait」虚偽コメントは残存** | ✗ (遅延回収は設計変更) |
| M-1 | swap_and_pop / remove の size==0 アンダーフロー | 残存 | ✅ 双方に範囲ガード |
| M-2 | mark_dirty の既マーク chunk 範囲拡張漏れ | 残存 | ✅ found 時に `end_object` を max 拡張 |
| M-3 | clear_dirty がカウンタをリセットしない | 部分修正 (per-frame 経路は `reset_frame_buffers()` で健全化済み) | ✅ `clear_dirty()` もカウンタリセット |
| M-4 | GpuMemoryAllocator 二重 free | 部分修正 (ring 側は `is_ring` 早期 return で解消)。mesh/ssbo プールの二重 free は残存 | ✗ (割当追跡が必要) |
| M-5 | sorted_indices_ の跨フレーム stale ポインタ | 残存 | ✅ build_dynamic 冒頭で null リセット + テスト |
| M-6 | RadixSort フレームメモリ枯渇時の silent 未ソート | 残存 | ✅ `std::stable_sort` フォールバック (post-condition 維持) |
| M-7 | dispatch の pending_tasks_ 上書き | **修正済** (fetch_add) | - |
| M-8 | hardware_concurrency()==0 ラップ | **修正済** (両所ガード) | - |
| M-9 | BVH rebuild のたび PoolAllocator 単調成長 | 残存 | ✗ (専用 allocator + clear 設計が必要) |
| M-10 | BVH 固定 64 段スタック飽和で部分木無音破棄 | 残存 | ✗ (線形フォールバック設計が必要) |
| M-11 | WorldPartition の swap-and-pop 不整合 | 残存 (static/dynamic の index 衝突も) | ✗ (モジュール間配線) |
| L-1 | peak_ の非 atomic データレース | 残存 | ✅ `std::atomic<size_t>` + relaxed CAS max-loop |
| L-2 | aligned_alloc のサイズ規格外 | 残存 | ✅ 64B 切り上げ |
| L-3 | NT store が同一アドレス load→stream | 残存 | ✗ (テンポラリ経由 or Level 2 削除の設計判断) |
| L-4 | get_stats の総数二重加算 | 残存 | ✅ 加算ループ削除 + テスト |
| L-5 | change_pool の customShader 消失 | **修正済 (再設計)** — shaderKey bit 63 + handle 運搬 (`types.h:333-360`) | - |
| L-6 | refit() が current_cost_ 未更新で needs_rebuild 恒久 false | 残存 | ✅ refit 末尾で更新 |
| L-7 | GpuAllocation::valid 未検査で initialized_=true | 残存 | ✅ 全 9 バッファ検証 + 失敗時 warn / 未初期化のまま |
| L-8 | MemoryConfig::use_large_pages が虚偽 API | 残存 (serializer は読むが runtime 未参照) | ✗ (runtime 経路 or フィールド削除の判断) |
| L-9 | SoAStream の default move で data_ 残留 | 残存 | ✅ `std::exchange` による明示 move |

**集計: 24 件中 — 6/11 時点から 6 件修正済み・4 件部分修正。今回残存分から 12 件を自動修正、7 件は設計作業として持ち越し。**

## 新規コード (4789398..8318501) の実装ギャップ

新規レビューの詳細は `REVIEW_DESIGN.md` §新規指摘を参照。実装バグとして今回修正したもの:

- **N-H1 GIBakeSystem UAF** (`renderer_subsystem_manager.cpp`): GI 無効プロファイルへの切替で参照先だけ破棄 → bake API で UAF。破棄順序 + 対の再生成を修正。
- **N-M5 DockLayout parser DoS** (`demo/graph/dock_layout.cpp`): 無検証カウント読取で OOM、循環 split で無限再帰。
- **N-L4 instance buffer null memcpy** (`demo/graph/vk_util.cpp`): `ensure_capacity` 失敗後の `upload()` が null に memcpy。
- **N-L5 viewport 未設定 draw** (`compiled_batch_recorder.cpp`): render_area=0 で GPU 実体だけ解決された場合に UB。skip 計上に変更 (headless 統計契約は維持)。
- **N-L8 SPIR-V 未検証ロード** (`demo/graph/vk_util.cpp`): codeSize % 4 / short read。

## 持ち越し (設計作業が必要 — 優先順)

1. **H-3 残り**: SSBO free-list の fence ベース遅延回収 + 「Fence wait」虚偽コメントの是正 (実 Vulkan 統合前に必須)
2. **N-M2**: recorder の pass 別バッチフィルタ (透明/不透明二重描画)
3. **M-9 / M-10**: BVH の allocator 設計 + トラバーサル飽和フォールバック
4. **M-11**: WorldPartition のプール index 衝突と swap-and-pop 伝播
5. **M-4 残り**: mesh/ssbo プールの二重 free 防御
6. **L-3 / L-8**: NT store の実装 or 削除、use_large_pages の実装 or API 削除

## テスト

- 追加: `tests/unit_batch_builder_test.cpp` — H-1 回帰 (最終 static バッチのキャッシュ漏れ)、L-4 (stats 二重加算なし)、M-5 (空フレームの stale indices なし)。
- 既存 18 テスト + 新規 1 = **19/19 パス** (ヘッドレス / Vulkan 有効の両構成でビルド確認)。
