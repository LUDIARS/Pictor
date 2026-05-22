# レンダリング拡張設計 (Rendering Extensibility Design)

> 2026-05-22 起草。KuzuSurvivors のレビューで判明した Pictor の構造的ギャップに対する 3 方針の設計書。
> 実装に先立つ設計の正本。関連: [`pipeline-profile-config.md`](pipeline-profile-config.md)、Ergo `spec/tool/render_pipeline.md`。

## 1. 背景・診断

Pictor のレンダリングは **2 系統に分断**されている（`pipeline-profile-config.md` §1）。

| 系統 | 実体 | 状態 |
|---|---|---|
| 系統A | `PipelineProfileDef`（宣言データ） | JSON で外部化済み (`*.profile.json`)、ツール編集可 |
| 系統B | 実 `VkRenderPass` チェーン (`vulkan_context.cpp` / `postprocess_pipeline.cpp`) | 完全ハードコード、系統A と未配線 |

KS の C++ 移植レビューで挙がった Pictor 側の不足:
- **カスタムシェーダ機構が無い** — マテリアルは固定 PBR、任意フラグメントシェーダの差し込み口が無い。
- **post-process がツール設定できない** — 編集 UI はあるがパラメータが C++ に届かず、実チェーンはハードコード。
- **パイプラインの可視化が DAG のみ** — 流れ（実行順）が直感的でない。

本書はこれに対する 3 方針の設計。共通原則: **系統A の拡張は容易、系統B（実描画）への配線が本質的作業**。各方針を「phase 1（設定・選択を可能に）」と「phase 2（系統B のハードコード解体）」に分ける。

## 2. 方針1 — カスタムシェーダを Visus で選択

### 現状の seam
`include/pictor/visus/visus.h` に既にカスタムシェーダ用の構造がある:
- `VisusGeometryKind::CUSTOM = 7`（shader override 用）
- `VisusDesc::asset`（CUSTOM 時は shader file を指す）/ `VisusDesc::shader`（`ShaderHandle`）/ `shader_key_override` / `VisusTextureSlot`
- `visus_serializer.cpp` は `shader` / `shader_key_override` を round-trip 済み。

→ **Visus データモデルは CUSTOM シェーダ参照を半分表現できている。**

### 欠けているもの
1. `VisusDesc::asset` がシェーダ「ファイル」を 1 つしか指せない（pipeline は最低 vert+frag が要る）。
2. `instantiate_visus` が `desc.shader` を `ObjectDescriptor` に伝播していない（`ObjectDescriptor` に shader フィールド自体が無い、`shaderKey: uint64_t` のみ）。
3. Pictor にシェーダ→`VkPipeline` の共通ロード経路が無い（各所でハードコードした `.spv` パスから個別生成）。
4. マテリアルは `BaseMaterialBuilder` の固定 PBR。

### 設計
**Visus を seam にする**（CUSTOM kind がそのために用意されている）。固定 PBR マテリアル層は温存し、カスタムシェーダは「マテリアルを置換」ではなく「CUSTOM kind の Visus が PBR 経路をバイパス」する形で両立させる。

- `VisusDesc` に CUSTOM 用シェーダステージ配列を追加（vert/frag/comp、各 `ResourceRef`）。
- Pictor に最小の `ShaderRegistry` + 「カスタムシェーダ → `VkPipeline`」生成経路を新設（`PostProcessPipeline` の pipeline 生成コードが雛形）。
- `ObjectDescriptor` に `ShaderHandle customShader` を追加（または `shaderKey` 上位ビットでカスタム識別）。
- `instantiate_visus` で `desc.shader` を `ObjectDescriptor` に伝播。
- Ergo `visus` プラグインに CUSTOM kind のシェーダ参照 UI（現状は生 JSON エディタ）。

- **phase 1**: 上記の差し込み経路（固定 vert+frag のカスタムシェーダを Visus から選択 → 描画に反映）。
- **phase 2**: シェーダ permutation / uniform バインドの作り込み、ShaderGraph 相当。

### phase 1 実装状況（2026-05-22, `feat/visus-custom-shader`）

| 項目 | 実体 | 状態 |
|---|---|---|
| シェーダステージ配列 | `VisusShaderStages`（vert/frag/comp 各 `ResourceRef`）を `VisusDesc::shader_stages` に追加 | ✅ |
| シリアライズ round-trip | `visus_serializer.cpp` の `geometry.shader_stages` ブロック | ✅ |
| `ObjectDescriptor` 拡張 | `ShaderHandle customShader` を追加 + `INVALID_SHADER` 定数 | ✅ |
| ShaderRegistry | `include/pictor/shader/shader_registry.h` + `src/shader/shader_registry.cpp`。`register_shader` / `build_pipelines`（`PostProcessPipeline` の graphics pipeline 生成が雛形）/ `pipeline(handle)` | ✅ |
| instantiate_visus 伝播 | CUSTOM kind かつ `desc.shader != INVALID_SHADER` のとき `customShader` 設定 + `ShaderKey::with_custom_shader` で `shaderKey` 上位ビットへ畳み込み | ✅ |
| 描画配線 | `shaderKey` 経由で `RenderBatch` → `DrawCommand::shader_key` まで自動伝播。ホストの記録ループは `ShaderKey::is_custom()` で判定し `ShaderRegistry::pipeline()` を引いて PBR pipeline の代わりにバインドする | ✅（データ経路完了。実 `vkCmdBindPipeline` はホスト責務） |

**ホスト側配線 (2026-05-22, KuzuSurvivors `feat/render-config-wiring`)**: KS は実描画に Pictor の `DrawCommand` 経路ではなく自前の `SkinnedRenderer` (固定 PBR pipeline) を使う。配線は KS 側の `SkinnedRenderer` が `ShaderRegistry` を所有し、`SkinnedLayer` が起動時に `data/visus/*.visus.json` の CUSTOM kind を走査・登録 → `build_pipelines`、`SkinnedDraw::shader_key` を持たせて `record()` の `vkCmdBindPipeline` 直前で `ShaderKey::is_custom()` を判定しカスタム pipeline へ切替える。カスタム pipeline は `SkinnedRenderer` の `pipeline_layout_` を流用するため descriptor set はそのまま有効。詳細は KS `spec/rendering_overview.md`「レンダリング設定の配線」。

`ShaderKey` ヘルパー（`core/types.h`）: `shaderKey` の bit 63 を CUSTOM フラグ、bit 32-62 を `ShaderHandle` に割当て、SoA stream を増やさずカスタムシェーダ識別を運ぶ。バッチビルダの sort key（`shader_keys >> 48`）は CUSTOM object を自然にグループ化する。

**phase 2 に残した範囲**: compute stage の pipeline 生成（`shader_stages.comp` はシリアライズのみ通過）、任意 uniform / descriptor バインド、シェーダ permutation。 mesh 駆動の頂点入力レイアウト確定は phase 2 項目2（§6.2）で実装済み。

## 3. 方針2 — post-process を含むレンダリングパスのツール設定

### 現状
- 編集 UI（render_pipeline プラグイン Profile Editor）は `*.profile.json` の `post_process[]` を CRUD 可能。
- C++ `PostProcessDef`（`pipeline_profile.h`）は `name` / `enabled` のみ。エフェクトのパラメータが C++ に届かない。
- 実 post-process は `PostProcessPipeline` に**固定 4-pass**（extract → blur H → blur V → grade）でハードコード。`PostProcessConfig`（各エフェクト構造体）は別系統で `set_config` 経由で渡される。

### 設計
- **phase 1（実装対象）**: `PostProcessDef` を拡張（`kind` + パラメータ、`PostProcessConfig` を受け皿に活用）→ `pipeline_profile_serializer` で round-trip → `PipelineProfileDef` の post-process スタック → `PostProcessConfig` → `PostProcessPipeline::set_config` のブリッジを実装。これで既存エフェクト（Bloom / ToneMapping / Vignette / ColorGrading(LUT) / DoF）のパラメータと on/off が**設定駆動**になる。Ergo 側は `profile_schema.ts` を型付きフィールド化 + Editor UI でパラメータ編集。
- **phase 2（別途）**: 任意の新規 post-process pass 挿入（SSAO / TAA / FXAA 等）。`PostProcessPipeline` の固定 4-pass・固定ターゲット数・固定 descriptor 構造の解体が必要。

### phase 1 ホスト側配線（2026-05-22, KuzuSurvivors `feat/render-config-wiring`）

KS の `PostProcessLayer::initialize` は `PostProcessConfig` をハードコードしていた。これを `data/render/kuzu.profile.json`（pipeline profile = 系統A）を `load_pipeline_profile_file()` でロードし、`build_post_process_config()`（`postprocess_config_bridge.h`）で `PostProcessConfig`（系統B）へ畳み込む経路へ差し替えた。プロファイル不在/破損時はハードコード既定にフォールバックして常にブート可能を保つ。起動後の `PostProcessTuner`（`data/postprocess.json` のライブ編集）はそのまま — プロファイルが「初期設定」、tuner が「実行時チューニング」の二層。詳細は KS `spec/rendering_overview.md`「レンダリング設定の配線」。

## 4. 方針3 — パイプライン/パスのタイムライン表示（実装済み 2026-05-22）

render_pipeline プラグインに 3 つ目のモード **Timeline** を追加（Scanner DAG / Timeline / Profile Editor）。
- 静的ガントチャート: topological level + レーン割当 + sub-pass ネスト + attachment ライフタイム。外部ライブラリ不使用。
- **phase 2**: GPU timestamp の WS 注入で実測タイムライン化 — ✅ 実装済み（2026-05-22, `feat/gpu-timestamp`）。詳細は §6.1。

## 5. 実装順序と phase 境界

| 順 | 方針 | 状態 | リポジトリ |
|---|---|---|---|
| 1 | 方針3 タイムライン | ✅ 実装済み（`feat/render-pipeline-timeline`） | Ergo |
| 2 | 方針2 post-process 設定化（phase 1） | ✅ Pictor 機能実装（`feat/postprocess-config`）+ KS 側配線完了（`feat/render-config-wiring`） | Pictor + KS |
| 3 | 方針1 カスタムシェーダ（phase 1） | ✅ Pictor 機能実装（`feat/visus-custom-shader`）+ KS 側配線完了（`feat/render-config-wiring`） | Pictor + KS |

- 方針2 を先に行う理由: 系統A↔B 接続の最初の実例になり、方針2 で作る「シェーダ/パイプライン生成経路」を方針1 が再利用できる。
- **phase 2（系統B のハードコード解体）は全方針で別タスク**。本フェーズは「既存の選択・設定」に限定し、`PostProcessPipeline` / pipeline 生成のハードコード構造の解体は含まない。

## 6. Phase 2 設計 — 系統B のハードコード解体（2026-05-22 起草）

Phase 1 は系統A↔B の「選択・パラメータ橋渡し」（`ShaderRegistry` / `build_post_process_config()` / `PipelineProfileDef`）を作った。Phase 2 は固定構造そのものを解体する。3 項目。

### 6.1 項目3 — GPU timestamp relay（実装済み 2026-05-22, `feat/gpu-timestamp`）
- **旧現状**: `GpuTimerManager`（`profiler/gpu_timer.*`）は完全な CPU シミュレーション（`vkCmdWriteTimestamp` / `VkQueryPool` が一切無く、コメントのみ。`value` 常に 0）。Ergo render_pipeline の timing UI（`app.js` の `injectTiming` / `applyTimingMessage`）は配線済み、`index.ts` の relay と Pictor の実装が残っていた。
- **設計**: `GpuTimerManager` を実 Vulkan 実装へ置換（`VkQueryPool` 作成 / `vkCmdWriteTimestamp` / `vkGetQueryPoolResults` / `timestampPeriod` 取得）。インタフェースと `flight_count` バッファリング枠は現状のまま中身差し替え。計測点は KS 側。timing を WS push する小経路を KS 側に新設。Ergo `index.ts` の `onUpgrade` に `{op:"timing"}` の `broadcast` を 1 行。pass ID は scanner の `PASS_DAG`（scene_hdr / decal_compose / postprocess / hud_load）に合わせる。
- 規模: 中、リスク: 低〜中（観測のみ、描画構造を壊さない）。

#### 実装内訳

| 項目 | 実体 | 状態 |
|---|---|---|
| `GpuTimerManager` 実 Vulkan 化 | `profiler/gpu_timer.{h,cpp}`。`initialize_vulkan()` が `flight_count` 枚の `VkQueryPool`（TIMESTAMP）を生成、`VkPhysicalDeviceProperties::limits.timestampPeriod` を取得。`begin_region`/`end_region`/`write_timestamp` に `VkCommandBuffer` 付きオーバーロード追加（`vkCmdWriteTimestamp`）。`reset_pool()` が `vkCmdResetQueryPool`。`collect_results()` が flight 遅延で `vkGetQueryPoolResults`（WAIT なし＝非ブロッキング、NOT_READY は据え置き）。`resolved_regions_` に確定結果を保持し `begin_frame` の clear に耐える。timestamp 非対応 GPU はシミュレーション経路へフォールバックしブート継続 | ✅ |
| 計測点（KS） | `GameRenderer` が `pictor::GpuTimerManager` を `KuzuRenderContext::gpu_timer` に所有。FrameComposer の `pre_pass_hook(0)`（frame begin）で `collect_results`→`begin_frame`→`reset_pool`→`begin_region("scene_hdr")`、`pre_pass_hook(1)`（HUD パス前）で scene_hdr 終了 + `decal_compose`/`postprocess` を実合成コマンドの前後で計測 + `hud_load` 開始、`HudLayer::record` 末尾で `hud_load`（route B は `scene_hdr`）終了。`FrameComposer` は無改変 | ✅ |
| timing WS push（KS） | `diagnostics/timing_relay.{h,cpp}`。`rive_player_ws_client`（既存 WS クライアント）を再利用し `ws://127.0.0.1:5170/render_pipeline/ws` へ `{op:"timing", frame, passes:[{id,us}]}` を `post_present` hook で push。未接続でも自動再接続、結果未回収（全 0）フレームは送らない | ✅ |
| Ergo relay | `render_pipeline/index.ts` の `onUpgrade` が `{op:"timing"}` を全 UI クライアントへ `broadcast` | ✅ |

**phase 2 に残した範囲**: route B（post-process 無効・単一 swapchain パス）は `decal_compose`/`postprocess` パスが構造的に存在しないため `scene_hdr` 1 本のみ計測（HUD 含む全体）。サブパス単位（post-process チェーン内の extract/blur/grade）の細分計測は項目1（任意 post-process pass 挿入）の解体と同時に行う。

### 6.2 項目2 — mesh 駆動の頂点入力レイアウト（実装済み 2026-05-22, `feat/mesh-vertex-layout`）
- **旧現状**: `ShaderRegistry::build_pipelines` が頂点入力空（`gl_VertexIndex` 前提）。カスタムシェーダがメッシュ頂点バッファを読めなかった。KS の `SkinnedRenderer::create_pipeline_` は `TexturedSkinnedVertex` をベタ書きしていた。
- **設計**: `VertexLayout`（`VertexAttribute[]` + stride）+ `→ VkVertexInput*Description` 変換ヘルパー。`CustomShaderDef` に `VertexLayout` フィールド追加。`build_pipelines` の `vi` 構築を実装。空レイアウト（phase 1 の `gl_VertexIndex`）をフォールバックで残す。Visus serializer に頂点レイアウト記述を追加。
- 規模: 中、リスク: 中（`layout(location=N)` 不一致は validation でしか出ない → まずは「def に明示記述・突き合わせは検証のみ」、SPIR-V reflection は将来）。phase 1 資産の再利用率が最も高い。

#### 実装内訳

| 項目 | 実体 | 状態 |
|---|---|---|
| `VertexLayout` 昇格 | 既に `data/vertex_data_uploader.h` に mesh 登録専用の `VertexLayout`（`VertexAttribute[]` + stride + `computed_stride()`）が存在したため、 新規定義を作らず `core/types.h` へ昇格して単一情報源に統合。 `vertex_data_uploader.h` 等の既存 consumer はそのまま共有定義を使う。 `VertexAttributeType::UINT32X4`（uvec4、 skinning joint indices 用）を追加 | ✅ |
| Vulkan 変換ヘルパー | `shader/vertex_layout.h`（Vulkan 依存ヘッダ）。 `to_vk_format()`（`VertexAttributeType` → `VkFormat`）、 `to_vk_vertex_input()`（`VertexLayout` → `VkVertexInputLayout`＝binding 1 本 + attribute 配列）。 空レイアウトは空の結果＝phase 1 互換フォールバック。 `location` は attribute 登録順を割当 | ✅ |
| `CustomShaderDef` 拡張 | `shader/shader_registry.h` の `CustomShaderDef` に `VertexLayout vertex_layout` を追加 | ✅ |
| `build_pipelines` 配線 | `src/shader/shader_registry.cpp` の `VkPipelineVertexInputStateCreateInfo{}` 空構築を `to_vk_vertex_input(def.vertex_layout)` ＋ `make_create_info()` へ置換。 空 `vertex_layout` のとき頂点入力空＝`gl_VertexIndex` 駆動のフォールバックを維持 | ✅ |
| Visus serializer | `VisusShaderStages` に `VertexLayout vertex_layout` を追加。 `visus_serializer.cpp` の `shader_stages` ブロックに `vertex_layout`（`stride` + `attributes[]`、 各 `{semantic,type,offset}`）を emit / parse 追加（JSON round-trip）。 `VertexSemantic`/`VertexAttributeType` の enum ↔ 文字列変換も追加 | ✅ |
| KS 統合 | `SkinnedLayer::register_custom_shaders_` が `desc.shader_stages.vertex_layout` を `CustomShaderDef` へ伝播。 `SkinnedRenderer` の `TexturedSkinnedVertex` レイアウトを `textured_skinned_vertex_layout()`（`VertexLayout`）で表現し直し、 `create_pipeline_` のベタ書きを `to_vk_vertex_input` 経由に統合（PBR pipeline と ShaderRegistry が同一の頂点入力構築経路） | ✅ |

**phase 2 に残した範囲**: SPIR-V reflection による `layout(location=N)` 自動突き合わせ（現状は def に明示記述、 不一致は validation layer 任せ）。 複数頂点バッファ / instance input rate（現状は単一 binding=0 / per-vertex のみ）。

### 6.3 項目1 — 任意 post-process pass 挿入（大規模・高リスク）
- **現状**: `PostProcessPipeline`（851 行）に固定 4-pass（extract→blur H→blur V→grade）・3 render pass・3 ターゲット・2 descriptor レイアウト・固定 push constant。`build_post_process_config()` が `PostProcessKind::UNKNOWN`（SSAO/TAA/FXAA 等）を黙って捨てている＝系統A→B の断点。
- **設計**: 固定構造を解体し、独立 `PostProcessChainBuilder` + 汎用 `PostProcessPassDef`（シェーダ `ResourceRef` + 入出力ターゲット名 + push constant レイアウト記述）を新設（`RenderPassDef` が雛形）。ターゲットを `std::vector<RenderTarget>` + 名前→index マップ化。subpass dependency を挿入順から動的生成。固定エフェクトは「組み込み pass テンプレート」として汎用チェーンに吸収。`ShaderRegistry::build_pipelines` と `PostProcessPipeline::create_pipelines_` の重複を統合。
- 規模: 大、リスク: 高（subpass dependency 動的生成の誤りで validation error / GPU hang、TAA history buffer はターゲットライフタイム >1 フレームで現 resize ロジックと非互換、push constant の汎用化）。
- `build_post_process_chain()`（`PostProcessDef[]` → `PostProcessPassDef[]`）を `build_post_process_config()` と並列に新設する。

### 6.4 Phase 2 実装順序
**3 → 2 → 1**（観測 → 局所改修 → 大規模解体）。項目3 を先に行うと項目1 の解体の影響（pass 数増減によるフレーム時間）をタイムラインで可視化しながら進められる。項目2 が項目1 の pipeline ビルダ統合の前提整理になる。

| 順 | 項目 | 状態 | リポジトリ |
|---|---|---|---|
| 1 | 6.1 GPU timestamp relay | ✅ 実装済み（`feat/gpu-timestamp`） | Pictor + KS + Ergo |
| 2 | 6.2 mesh 駆動の頂点入力レイアウト | ✅ 実装済み（`feat/mesh-vertex-layout`） | Pictor + KS |
| 3 | 6.3 任意 post-process pass 挿入 | 未着手 | Pictor + KS |
