# Iter Relation Graph — native renderer 引継ぎ資料

`demo/graph/` (target `pictor_graph_demo`) の設計・進捗・残作業の引継ぎ。
2026-06-25 時点。正本メモ: `~/.claude/projects/.../memory/project_iter_native_graph_renderer.md`。

## 1. 目的 / 背景

[Iter](https://github.com/LUDIARS/Iter)（C/C++ の caller/callee/reference を clangd で
グラフ可視化する Tauri デスクトップエディタ）のグラフビューは、Web の React Flow
(`@xyflow`) を使っており **大規模グラフで描画が破綻**する。これを **native (Pictor)**
へ持ち上げ、「描画量を視界に比例させる (SoA × LOD × instancing)」設計で
1 万〜数万ノードでも溶けない graph widget を作るのがこのデモの狙い。

戦略上の位置づけ:
- **descriptor が契約**: Corpus 宣言的レンダリング (`PanelDescriptor`) を不変フォーマット
  として C++ も同じ JSON を食う。意味等価を狙い、ピクセル等価は狙わない。
- **2-tier**: Tier A=宣言的 component(Web と共有)、**Tier B=native widget**(viewport/graph)。
  この graph widget が **Tier B 第1号**。Monaco はそのまま Web、グラフだけ native に落とす。

## 2. アーキテクチャ (現在のデモ)

```
main.cpp                window + frame loop + 入力ルーティング
  └ DockLayout          split/tabs/leaf ツリー (Vulkan 非依存)。graph は leaf の1つ
  └ UiRectRenderer      dock chrome (パネル背景/splitter/タブ) = screen-space solid rect
  └ BitmapTextRenderer  (pictor lib) 8x16 monospace。タブ名 + ノード LABEL/snippet
  └ GraphView           ★ Tier B widget 本体
       ├ GraphStore      SoA (cx/cy/w/h/rgba/kind/flags/label)。append-only
       ├ SpatialGrid     CSR 一様グリッド。query_visible=O(visible) cull / pick=hit-test
       ├ Camera2D        pan/zoom (world↔screen、cursor 固定ズーム)
       ├ LayoutEngine    Sugiyama 層状レイアウト (worker スレッドで計算)
       ├ SnippetCache    NEAR-LOD snippet の bounded LRU (遅延取得)
       └ GraphRenderer   per-flight 動的バッファ + region(scissor) instanced 描画
```

per-frame の流れ (GraphView::render):
1. camera から view AABB → `SpatialGrid::query_visible` で可視ノード
2. 可視ノードのみ NodeInstance を組み、edge は端点可視を frame-stamp で判定
3. `GraphRenderer::draw` が flight の動的 SSBO へ upload → node/edge 各 1 draw (leaf に scissor)
4. `draw_labels` が LOD で text 描画: 小=なし / 中=シンボル名 / 大(≥200px)=snippet カード

描画は anim 位置 (レイアウト lerp 中)、cull/hit-test は target 位置 (grid)。

## 3. 進捗

| Step | 内容 | PR | commit |
|---|---|---|---|
| 1 | GraphStore(SoA)+Camera2D+instanced node/edge | #89 | a743c88 |
| 2 | SpatialGrid cull + hit-test + per-flight 動的描画 | #90 | 09ba4a3 |
| d | native dock (split/tabs/leaf, splitter ドラッグ, タブ, 永続) | #90 | 09ba4a3 |
| 3 | Sugiyama layered layout (worker + lerp 整列) | #91 | 993b312 |
| 4 | テキスト + LABEL LOD (BitmapTextRenderer 再利用) | #92 | 2a69d46 |
| 5 | NEAR-LOD snippet カード + LRU 遅延取得 | #93 | 63479ff |
| 6 | incremental expand + lerp / IDataChannel / edge cull / dock desc / click-jump 抑制 | #397/#398/#400 | feat/pictor-397-400 |

LOD 描画パイプライン（点 → 名前 → コード）は完成。Step 6 で **クリック展開**
(IDataChannel 経由で子取得→`id_to_index` で append-only マージ→親位置から lerp、
GPU バッファ動的再確保) と既知課題 (edge cull / dock descriptor / click-jump) を解消。

## 4. ファイルマップ (`demo/graph/`)

| ファイル | 役割 |
|---|---|
| `main.cpp` | window/loop/入力、dock 解決→graph leaf 配置、text バッチ |
| `graph_store.{h,cpp}` | SoA ストア (append-only)、`set_positions`、`label`、`id`/`find`/`id_of` (expand dedup) |
| `data_channel.h` | `IDataChannel` + `{nodes,edges}` 契約 (GraphData/Node/Edge desc)。データ源境界 |
| `synthetic_data_channel.{h,cpp}` | 決定的合成 `IDataChannel` (initial + request_children) |
| `clangd_data_channel.{h,cpp}` | clangd callHierarchy 契約ミラー + `{nodes,edges}` 変換 + 注入 fetch 境界 (Iter IPC) |
| `graph_generator.{h,cpp}` | 決定的合成グラフ + シンボル名。`--scatter` で初期散布 |
| `spatial_grid.{h,cpp}` | CSR 一様グリッド (cull / pick) |
| `camera2d.{h,cpp}` | pan/zoom カメラ、world↔screen 行列 |
| `layout_engine.{h,cpp}` | Sugiyama (cycle 除去→層割当→barycenter→座標) |
| `graph_view.{h,cpp}` | Tier B widget。cull/hover/layout worker/lerp/draw_labels |
| `graph_renderer.{h,cpp}` | instanced node/edge、per-flight 動的 SSBO、region 描画 |
| `vk_util.{h,cpp}` | 共通 Vulkan ヘルパ + `DynamicInstanceBuffers` |
| `ui_rect_renderer.{h,cpp}` | dock chrome の screen-space solid rect |
| `dock_layout.{h,cpp}` | split/tabs/leaf ツリー、solve/drag/tab/永続 |
| `snippet_cache.{h,cpp}` | bounded LRU (list+map) |
| `graph_instances.h` | Node/Edge/Ui インスタンス構造体 (共有) |
| `shaders/` | graph_node.vert / graph_edge.vert / graph_flat.frag / ui_rect.vert |

## 5. ビルド & 実行

```bash
cmake -S . -B build -DPICTOR_BUILD_TESTS=OFF
cmake --build build --target pictor_graph_demo --config Debug
# 実行 (Pictor 規約: ビルドまでが AI、実行はユーザ側):
build/Debug/pictor_graph_demo.exe --nodes 20000 --scatter
```

操作: Drag(graph内)=pan / Scroll=zoom / splitter ドラッグ=分割比 / タブクリック=切替 /
R=再フィット / S=dock 保存 / Esc=終了。引数 `--nodes N` `--frames N` `--scatter`。
FPS 行に `visible u/total`・`zoom`・`hover`・`snip$`(キャッシュ件数) が出る。

## 6. 残作業 (Memoria タスク登録済み)

### 解決済み (Step 6 / feat/pictor-397-400)

- **#397 incremental expand + lerp** — ノードクリック (press+release without drag) で
  `IDataChannel::request_children`→`GraphStore::find` (`id_to_index`) で append-only
  マージ→新ノードは親の anim 位置から fan-out 目標へ lerp。総数増加時は
  `GraphRenderer::ensure_capacity`→`DynamicInstanceBuffers::ensure_capacity` で per-flight
  バッファを device idle 後に再確保 (descriptor set はそのまま rebind、pool 枯渇なし)。
- **#398 IDataChannel + clangd 契約** — `data_channel.h` に `{nodes,edges}` 契約と
  `IDataChannel`。`SyntheticDataChannel` (合成) と `ClangdDataChannel` (clangd
  callHierarchy ミラー→GraphData 変換、`std::function` fetch が Rust(Iter)↔C++(Pictor)
  IPC 境界)。Pictor は Tauri/serde 非依存のまま。**実 IPC 配線 (Iter Rust 側の
  `graph_request_children` コマンド + fetch 注入) は未着手** — Pictor 側の型と境界は確定。
- **#400 edge cull** — `segment_intersects_rect` (Liang-Barsky) を追加。両端点 off-screen
  でも線分が view を横切るエッジを描画。
- **#400 dock_layout** — `DockDesc` (Corpus `DockLayoutNode` ミラー: leaf/split/tabs) +
  `DockLayout::set_layout` で宣言的ツリーから構築。`set_default` も set_layout 経由に。
- **#400 click-jump 抑制** — ノードクリックで `select`+`expand_node` するが camera は
  一切動かさない (fit/reset 呼ばない)。drag (slop 超え) は pan のみで選択しない。

### 未着手

- **clangd 実 IPC 配線** — Iter Rust に `graph_request_children` を追加し、
  `ClangdDataChannel` の fetch にバインド。Iter の OS 子窓を Pictor native surface 化し
  Monaco/ControlPanel は Web のまま合成。
- **実行時検証 (確認作業)** — exe 起動し expand lerp / edge cull / dock / hover / layout を
  目視確認 (Pictor no-run のため未実施)。
- **dock**: floating / closable / ドラッグ再ドック / 新規分割生成 (現状 resize+タブ+宣言構築)。
- **ノード source ジャンプ** 未配線 (select highlight + expand のみ)。
- **Tier A 宣言的レンダラ** (corpus-renderer-native 本体) は未着手 (概念のみ)。

## 7. 設計判断 / 落とし穴

- **DoD**: hot path は handle index のみ。symbol 文字列・id map は cold。node 配列は
  append-only で handle 安定 (アニメ/expand 前提)。
- **per-flight 動的バッファ**: 可視サブセットを flight ごとの host-visible mapped buffer へ
  毎フレーム memcpy。`acquire_next_image` の fence が flight 再利用を gate (書込安全)。
- **レイアウトは worker**: per-frame は `pos[]` を読むだけ。anim を lerp、settle で O(N) 停止。
- **LABEL/snippet は LOD**: 描画量を視界に比例。snippet は NEAR のみ + LRU で resident 有界。
- **テキストは BitmapTextRenderer 再利用** (8x16 埋め込みフォント)。glyph atlas 自作せず。
  数千ラベル規模が要るなら TextRun キャッシュ化を検討。
- **Pictor 規約**: 上位ライブラリ非依存 / DoD / MSVC は demo target に `/utf-8` 個別付与
  (lib から非伝播) / 実行はユーザ側 / device リソース teardown 順序注意。
- **layout_engine の罠 (修正済)**: サイクル除去で back-edge を CSR item index で記録し
  原配列 index で読む不整合があった → acyclic 辺を DFS 中に直接構築する形に修正。
- **git**: 新規ファイルは LF→CRLF 正規化 (blob は LF)。worktree 隔離で作業し、merge は
  リポ dir から (worktree 撤去後)。
