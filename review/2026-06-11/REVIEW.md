# AI Code Review — Pictor v2.1 (Fable 診断)

| 項目 | 値 |
|------|-----|
| リポジトリ | LUDIARS/Pictor |
| 対象ブランチ / PR | main (HEAD: 4789398) |
| レビュー実施日 | 2026-06-11 |
| 対象範囲 | **コードベース全体** (src/ 40,026 行 + tools/level-editor)。前回 (2026-05-17) の差分レビューと異なり、Fable 指標による全量再診断 |
| レビュー方式 | 4 領域並列深掘り (脆弱性 / メモリ・並行性 / 設計 / 品質・堅牢性)。全指摘は実コード読解で file:line 検証済み |
| レビュアー視点の記録 | [FABLE_PERSPECTIVE.md](FABLE_PERSPECTIVE.md) |

---

## 総合評価 (Overall Assessment)

| # | レビュー観点 | 評価 | 前回 | 重大指摘数 (Critical/High) | ドキュメント |
|---|------------|------|------|------|------------|
| 1 | 脆弱性 | **B-** | A | 0 / 3 | [脆弱性レビュー](REVIEW_VULNERABILITY.md) |
| 2 | 設計強度 | **B+** | A | 0 / 1 | [設計レビュー](REVIEW_DESIGN.md) |
| 3 | 設計思想の一貫性 | **B** | A | 0 / 2 | [設計レビュー](REVIEW_DESIGN.md) |
| 4 | モジュール分割度 | **A-** | A | 0 / 0 | [設計レビュー](REVIEW_DESIGN.md) |
| 5 | コード品質 | **B-** | B | 1 / 3 | [実装評価](REVIEW_IMPLEMENTATION.md) |
| 6 | データスキーマ | A- | A | 0 / 0 | [実装評価](REVIEW_IMPLEMENTATION.md) |
| 9 | SRE (運用堅牢性) | **B** | A | 0 / 3 | [品質保証レビュー](REVIEW_QUALITY.md) |
| 10 | ゼロトラスト | N/A | N/A | - | [脆弱性レビュー](REVIEW_VULNERABILITY.md) |
| 11 | セキュリティ | **B-** | A | 0 / 3 | [脆弱性レビュー](REVIEW_VULNERABILITY.md) |
| 12 | テスト戦略・カバレッジ | **B-** | B | 0 / 0 | [品質保証レビュー](REVIEW_QUALITY.md) |
| 13 | パフォーマンス | B | B | 0 / 1 | [設計レビュー](REVIEW_DESIGN.md) |
| 14 | ライセンス遵守 | A (前回値継続・今回未再査) | A | - | - |
| 15 | クロスプラットフォーム互換 | **B** | A | 0 / 1 | [品質保証レビュー](REVIEW_QUALITY.md) |
| 16 | ドキュメント完備性 | A- | A | 0 / 0 | [品質保証レビュー](REVIEW_QUALITY.md) |

**加重総合: B** (前回 A-)

> スコア低下は品質後退ではなく**測定方法の変化**による。過去レビューは直近コミット差分中心だったのに対し、今回は全 40k 行を信頼境界・並行性・「コメントと実装の乖離」を軸に深掘りした。発見された問題の多くは初期実装から存在していたものである。

---

## 重大指摘サマリー

**Critical (1):**

- **[C-1] ThreadPoolDispatcher の lost wakeup → レンダースレッド永久ハング** — `src/update/job_dispatcher.cpp:49-54,76-78`。worker が mutex 非保持で `pending_tasks_` をデクリメントし `notify_all()` するため、`wait_all()` の述語評価と `wait()` 突入の間に通知が消失し得る。`UpdateScheduler::update()` 経由で**毎フレーム**この競合ウィンドウを踏む。

**High (12):**

| # | 領域 | 指摘 | 場所 |
|---|------|------|------|
| V-1 | 脆弱性 | TrueType glyf パーサのヒープ OOB read (total_pts 巻き戻り / ends 非単調) | `src/text/text_rasterizer.cpp:128,184-234` |
| V-2 | 脆弱性 | フォントテーブル/グリフオフセットの uint32 加算オーバーフローで境界チェック迂回 | `src/text/font_loader.cpp:225,270` / `text_rasterizer.cpp:92-114` |
| V-3 | 脆弱性 | FileSystemResourceLoader のパストラバーサル (任意ファイル読取) | `src/visus/resource_loader.cpp:29-58` |
| M-1 | メモリ | BatchBuilder: static バッチが 2 フレーム目以降消失 (dirty キャッシュと clear の矛盾) | `src/batch/batch_builder.cpp:10-23` |
| M-2 | メモリ | FrameAllocator move 代入が large-pages ビルドで解放関数ミスマッチ → ヒープ破壊 | `src/memory/frame_allocator.cpp:94-102` |
| M-3 | メモリ | GPU リング(称)バッファに in-flight フレーム保護なし。「Fence wait」コメントは虚偽 | `src/memory/memory_subsystem.cpp:17-23` / `pictor_renderer.cpp:142-144` |
| Q-1 | Vulkan | acquire 失敗 (OUT_OF_DATE) 時に fence が unsignaled のまま → 次フレーム永久 wait | `src/surface/vulkan_context.cpp:107-118` |
| Q-2 | Vulkan | iOS surface 作成 case 欠落 — iOS ビルドは必ず初期化失敗 (実質スタブ) | `vulkan_context.cpp:202-289` vs `ios_surface_provider` |
| Q-3 | Vulkan | frames-in-flight 1 固定 + render_finished semaphore の全 image 共有 | `vulkan_context.cpp:679-694` |
| D-1 | 設計 | PictorRenderer God Class 未解消 (前回 Critical、18 unique_ptr / 12 責務群) | `include/pictor/core/pictor_renderer.h:262-279` |
| D-2 | 設計 | managed 描画経路の大部分がスタブで `get_frame_stats()` が虚偽の値 (draw call 常時 0) を返す | `render_pass_scheduler.cpp:39-98` / `gpu_driven_pipeline.cpp:52-80` |
| D-3 | 設計 | Profiler/GpuTimer の per-frame std::string 確保・線形比較 (DoD 規約違反、前回 High 継続) | `src/profiler/profiler.cpp:37-101` / `gpu_timer.cpp:113-159` |

**前回指摘 (2026-05-30 規約レビュー) の追跡: 9 件中 0 件修正。** 詳細は [REVIEW_DESIGN.md](REVIEW_DESIGN.md) §前回指摘の追跡。

---

## 総合所見

**強み (本物の部分):**
- **DoD 中核は規約の理想形**: ObjectPool の完全 SoA + hot/cold 分離、swap-and-pop、BatchBuilder/CullingSystem の per-frame 確保が全て FrameAllocator 経由 (frame 内 heap alloc ゼロ)、`float4x4` の cache-line align + static_assert。
- **pipeline 再設計 (PipelineCompiler / 3 レジストリ / CompiledGraph) は良設計**: hot-path 不変条件 (string/map ゼロ) がヘッダに明文化され、実装も宣言どおり。所有権コメントも明確。
- **レイヤ健全性**: 公開ヘッダの include 循環 0、上位ライブラリ実依存 0 (コメント残存のみ)、test target の MSVC `/utf-8` 対応済み。
- **インタフェース分離**: ICuller/IBatchPolicy/ISurfaceProvider 等の細粒度分離、Material kind は registry 登録 (OCP 遵守)。
- パーサにも良い防御が点在: FBX DEFLATE の距離検証、fbx_scene の全インデックス範囲チェック、C API の null チェック一貫性。

**今回浮き彫りになった構造的課題 (3 テーマ):**
1. **「コメントが約束する安全性」と実装の乖離** — fence wait と書いてあるのに無い (M-3)、ring と称して全域リセット、「Update to temporary first」なのに同一アドレス store。コメントを信じると壊れる箇所が複数。
2. **二重アーキテクチャの残置** — 実描画は host-driven (CompiledGraph) に移行済みなのに、旧 managed 経路がスタブのまま「動いているように見える」形で残り、統計 API が虚偽値を返す (D-2)。削除か明示的隔離が必要。
3. **信頼境界の未定義** — フォント / FBX / BVH / visus JSON は「攻撃者が用意し得るファイル」として扱われておらず、OOB read・パストラバーサル・DoS (zip bomb / 無制限再帰 / 過大確保) が系統的に存在する。

**推奨アクション順:**
1. C-1 (lost wakeup) — 修正は数行。毎フレーム踏む競合のため最優先
2. Q-1 (fence デッドロック) + M-2 (free ミスマッチ) — 同じく小修正で High 解消
3. V-1/V-2 (フォント OOB) + V-3 (パストラバーサル) — 信頼境界の確立
4. D-2 (スタブ経路の削除 or 隔離) — 虚偽統計の解消
5. M-1 (static バッチ消失) — 機能バグとして顕在化必至
6. D-1 (God Class 分割) / D-3 (Profiler enum 化) — 前回からの持ち越し
