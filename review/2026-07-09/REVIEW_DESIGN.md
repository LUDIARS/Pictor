# 設計レビュー — Pictor (2026-07-09, Fable 診断)

規約基準: `CLAUDE.md` (DoD hot path / OOP 境界 / SOLID 全 5 / 最下層レイヤ)。
前回: `review/2026-06-11/REVIEW_DESIGN.md` (HEAD 4789398)。今回対象: HEAD 8318501 (前回比 23 コミット / +10,484 行 — compiled 経路配線 #97、graph-demo Step2-6、D-1/D-2/D-3 是正 #80 を含む)。

## サマリ

- **前回指摘 14 件中: 2 件修正 (D-1 / M-2)、1 件対象消滅 (L-4: ファイル削除)、2 件部分修正 (D-2 / D-3)、9 件残存** (うち L-2 は今回 autofix)。前回「9 件中 0 件修正」だった停滞は解消し、High 帯はすべて着手された。
- **D-1 (God Class) は推奨どおりの 3 分割 (SubsystemManager / MobileLifecycleController / GIFacade) で解消**。PictorRenderer は facade 委譲のみ (569 行、旧 669 行)。
- **D-2 の中核 (虚偽統計) も解消**: CommandEncoder 削除、draw call / triangle は `CompiledBatchRecorder` の実測値、未実装経路は「未計測 = 0」+ 一回限りの明示 warn。OverlayRenderer スタブと GPU dispatch 未実装は「正直なスタブ」として残存。
- 新規コードの筆頭リスクは **プロファイル切替時の GIBakeSystem ダングリング参照 (N-H1)** — 本レビューで自動修正済み。

## 前回指摘の追跡

| ID | 指摘 | 状態 | 証拠 |
|---|---|---|---|
| D-1 | PictorRenderer God Class | **修正済** | `renderer_subsystem_manager.h:73-89` に所有権移管、mobile/GI を各 controller/facade へ切り出し |
| D-2 | managed 経路スタブ + 虚偽統計 | **部分修正 (中核解消)** | CommandEncoder 削除、stats は recorder 実測。OverlayRenderer (`overlay_renderer.cpp:10-14`)・GPU dispatch (`gpu_driven_pipeline.cpp:51-60`) は正直なスタブとして残存 |
| D-3 | Profiler/GpuTimer per-frame string | **部分修正** | CPU 側 enum 化・GPU 側の毎フレーム連結ヒープ確保は解消。`gpu_timer.cpp:126,171,259` の per-frame strcmp 線形走査は残存 |
| M-1 | WorldPartition unordered_map | 残存 | `world_partition.h:95-98` / `query_frustum` の map 全走査そのまま |
| M-2 | remap_batches_for_pass の per-frame vector | **修正済 (コード削除)** | `render_pass_scheduler.cpp:141-144` の墓標コメント。解決は recorder のインライン方式へ |
| M-3 | PipelineCompiler framebuffer 位置依存解決 | 残存 | `pipeline_compiler.cpp:163` は名前解決 vs `:182` は位置 `i` — 暗黙契約のまま。headless ガードのみ追加 |
| M-4 | umbrella ヘッダの Vulkan/GLFW 伝播 | 残存 (悪化) | `pictor.h:64-66` 無条件 include。vulkan.h を引く公開ヘッダ 16 → 22 に増加 |
| M-5 | SkinnedVertex バッファ側 alignment | 残存 | `pack_skinned_vertices` は default allocator の `std::vector` のまま |
| M-6 | 巨大ファイル | 残存 | serializer 1453 行 (微増)、fbx_viewer 2102 行ほか |
| L-1 | 上位ライブラリ名の混入 | 残存 (増加) | 是正コミット自体が「PrivateGame 参照」等の新規言及を追加 (~22 箇所)。実依存は引き続きゼロ |
| L-2 | object_pool.h 未使用 include | **修正済 (今回 autofix)** | `<unordered_map>` を除去 |
| L-3 | c_api の per-call 台帳 + no-op render_frame | 残存 | c_api.cpp はビルド対象化された (`CMakeLists.txt:210`) が中身は Phase 2 待ち |
| L-4 | command_encoder 未使用引数 | 対象消滅 | ファイル削除済み |
| L-5 | custom pass の per-frame 文字列比較 | 残存 | `render_pass_scheduler.cpp:33` |

## 新規指摘 (4789398..8318501 の変更分)

### High

**N-H1. プロファイル切替で GIBakeSystem がダングリング参照を抱える** — **自動修正済み**
`renderer_subsystem_manager.cpp` の `apply_profile()` が GI 無効化時に `gi_system_` だけ reset し、`GILightingSystem&` を保持する `bake_system_` を生かしたままにしていた。GI 有効 → `set_profile(GI無効)` (thermal auto-downgrade 含む) → bake API で use-after-free。修正: bake → GI の順で対で破棄し、GI 有効化側でも bake を対で再生成 (N-M1)。

### Medium

- **N-M2. 既定 recorder が OPAQUE / TRANSPARENT 両パスで全バッチを描画** — `compiled_batch_recorder.cpp` は `*batches_` を無フィルタで走査し、compile 済みの `CompiledPass::filter_mask` / `sort_mode` は未消費。両パスを含むプロファイルで全バッチ二重描画 (ブレンド誤り + 統計 2 倍)。`RenderBatch` に透明フラグがなく安全な局所修正が組めないため**未修正** — Phase 4 で `RenderBatch` へ transparency bit を運ぶか、`IBatchGpuSource::resolve()` の契約に「対象外 pass は false」を明文化すべき。
- **N-M3. recompile 失敗の握り潰し** — **自動修正済み**。`apply_profile` が `recompile()` の戻り値を捨て、失敗時に空 graph のまま engaged が続いていた。失敗時は明示ログ + disengage に変更。
- **N-M4. descriptor pool 破棄に GPU idle 契約なし** — **自動修正済み (保守的)**。thermal auto-downgrade 経由の recompile は描画中にも走るため、旧 graph 解放前に `vkDeviceWaitIdle` を挟む (`compiled_path_driver.cpp: release_old_graph_`)。Phase 4 で fence ベースの遅延破棄に置き換え可。
- **N-M5. DockLayout::load のパーサ DoS** — **自動修正済み**。`tabs 0 9999999999` で OOM、自己参照 split で `solve_node()` 無限再帰。読取検証 + 上限 64 + 到達検査 (循環棄却) を追加。同レンジで `parse_limits.h` を導入した方針との整合。

### Low (自動修正済みのもの)

N-L1 shutdown 順序 (DataHandler → AnimationSystem)、N-L2 snippet の per-frame 全文コピー + substr 確保、N-L3 chrome vector の per-frame 確保、N-L4 instance buffer 拡張失敗の null memcpy、N-L5 render_area=0 で viewport 未設定 draw、N-L6 auto-downgrade の空文字 sentinel、N-L8 SPIR-V サイズ/short-read 未検証。

### Low (未修正)

- **N-L7**: `clangd_data_channel.cpp:64` — `clangd_node_id(r.uri, line, r.uri)` と uri が name 引数にも渡り、参照ノードが無ラベル。意図確認が要るため保留。

## 強み (前回から前進した点)

1. **正直な計測への一貫姿勢**: 未実装は「無言 no-op」でなく「一回警告 + 未計測 0 + 専用カウンタ」。recorder の統計は実 vkCmd 呼び出しの計数。
2. **compiled 経路の配線が完成**: compile→install→recompile→disengage のライフサイクルを `CompiledPathDriver` が単責務で持ち、342 行の headless 統合テスト付き (前回「統合テスト不在」指摘への回答)。
3. **frames-in-flight 再設計**: per-image fence 追跡、acquire 成功後の fence reset (resize デッドロック解消コメント付き)。
4. **graph demo は DoD の実践例**: SoA store + cold string 分離、CSR spatial grid、Liang-Barsky エッジカリング、有界 LRU、上位依存ゼロ。

## 推奨アクション順 (次回)

1. **N-M2**: RenderBatch への transparency bit 追加 or resolve() 契約明文化 — 両パスプロファイルで即顕在化する
2. **M-3**: framebuffer 解決の名前ベース統一 (compile 時エラー化)
3. **M-4**: `pictor_vulkan.h` opt-in umbrella の分離 (露出 22 ヘッダに拡大中)
4. **M-1**: WorldPartition flat grid 化 (本格利用前)
5. **D-3 残り**: GpuTimer の region を enum / index 引きに (per-frame strcmp 除去)
