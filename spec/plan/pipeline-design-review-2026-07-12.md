# レンダリングパイプライン設計レビュー実装資料 (2026-07-12)

> 2026-07-12 起草。PR #99 (パイプライン途中編集 API + Low/Mid/High プリセット) と同時に実施した
> レンダリングパイプライン全体の設計レビューを、次の実装作業の資料として整理したもの。
> 前回レビュー: [`review/2026-07-09/`](../../review/2026-07-09/REVIEW.md)。既知指摘は状態のみ記載し重複させない。
> 関連: [`rendering-extensibility-design.md`](../feature/rendering-extensibility-design.md)、
> [`pipeline-system-b-config.md`](../feature/pipeline-system-b-config.md)。

## 1. 総評

系統A (宣言 profile) → 系統B (compiled flat 実行) の 2 層構造は健全。hot path の flat 化、
名前解決の compile 時一括化、「無言 no-op 禁止」(warn + 専用 stat、§7.1) は一貫している。

主要リスクは 2 つ:

1. **宣言スキーマが実装より大きく先行** — profile の約半分のフィールドがランタイム未消費。
2. **registry ↔ CompiledGraph の再構築を束ねる調整層の不在** — resize / 差し替えの順序が
   暗黙契約のままで、dangling VkHandle の窓がある。

## 2. 宣言と実体のギャップ (現状マップ)

| 宣言 | 実体 | 根拠 |
|---|---|---|
| `rendering_path` (FORWARD_PLUS 等) | 未消費。ランタイム分岐ゼロ | 消費点は serializer / builder の round-trip のみ |
| `max_lights` | 未消費。動的ライト配列自体が無い (実ライトは directional 1 灯 `gi_lighting_system.h:244`) | `PointLight` は GI bake 用データ型のみ |
| `msaa_samples` | 未消費。VkPipeline multisample state に反映されない | `pipeline_compiler.cpp` は multisample を profile から組まない |
| `LightCullPass` (High プリセット) | compute shader / dispatch とも存在しない。host record 前提の空 COMPUTE pass 宣言 | `shaders/` に light cull / tile / cluster 系 `.comp` なし |
| SSAO / Shadow / GI probe | CPU 計算 (カスケード行列等) まで実装、GPU 発行は全てコメントアウト | `gi_lighting_system.cpp:266-399` の各 `execute_*` |
| `post_process_stack` | `apply_profile()` は触れない。`PostProcessPipeline` は PictorRenderer 非統合、リポジトリ内プロダクション呼び出し元なし (tests のみ)。`demo/postprocess` も実チェーン不使用 (自前シェーダ近似) | `pictor_renderer.h:295-298` に host 責務と明記 |
| `memory_config` | initialize 時のみ。profile 切替で反映されない (flight_count を変えても効かない) | `renderer_subsystem_manager.cpp:16` |

補足:

- Forward+ は現状「pass 列の宣言」であって実装ではない。High プリセットの `LightCullPass` /
  `SSAOGen` は Standard の `HiZBuild` / `GPUCullPass` と同種の Phase 4 待ち宣言で、プリセット間の
  整合は取れているが実体はない。
- High の DoF はチェーン実装が存在する (`build_post_process_chain()` の dof pass +
  `__depth__` 入力 + `dof.frag`)。ただし host が `PostProcessPipeline::initialize_vulkan()` /
  `record()` を駆動して初めて動く。

## 3. 指摘一覧

### High

**H-1. GI 系の正直性の非対称 (§7.1 違反)**
GPU-driven パイプラインは「未実装は warn + 未計測 0」に是正済み (2026-06-11 D-2) だが、
GI 系 (`gi_lighting_system.cpp` の `execute_shadow_pass` / `execute_ssao_pass` /
`execute_gi_probe_pass`) は **dispatch を発行していないのに `shadow_casters` 等の stats へ
見かけ上の値を書き、warn も出さない**。プロファイラを信じた性能判断を誤らせる。
`upload_probe_data` も SH データを捨てて `stats_.active_probes` だけ書く (`:252-256`)。

**H-2. resize 時の VkFramebuffer dangling 窓 (順序依存の暗黙契約)**
`PipelineCompiler` は framebuffer handle を `CompiledPass` へ値コピーする
(`pipeline_compiler.cpp:181-183`)。host が `FramebufferRegistry::resize()` (旧 fb を destroy、
`framebuffer_registry.cpp:145-148`) を呼んで再 compile を忘れると、次の `execute_compiled` が
破棄済み framebuffer で `vkCmdBeginRenderPass` → use-after-free。
「atts.resize → fbs.resize → 再 compile」の 3 段順序を強制する調整層がない。
`AttachmentRegistry::set_defs` / `RenderPassRegistry::set_passes` の内部 `shutdown_vulkan` も
他 registry / CompiledGraph へ伝播しない。

**H-3. filter_mask 無視による opaque/transparent 二重描画 (既知 N-M2 残存)**
compile は `cp.filter_mask` を格納する (`pipeline_compiler.cpp:157`) が、
`CompiledBatchRecorder::record_batches_` は `*batches_` を無フィルタ走査
(`compiled_batch_recorder.cpp:100-151`)。OPAQUE / TRANSPARENT 両 pass を含む profile
(Lite/Standard/Ultra/Low/Mid/High 全て) で全バッチ二重描画。`sort_mode` も宣言のみ未適用
(`compiled_batch_recorder.cpp:96-97` が自認)。前回レビュー最優先アクションのまま。

### Medium

**M-1. pass の安定 ID が無い — Forward+ 実装の前提欠落**
hot path の pass 識別子は `pass_type` (uint8) のみで、`debug_name` は「hot path で読まない」
契約 (`compiled_graph.h:66-68`)。COMPUTE pass が 2 つ以上ある profile (High の LightCullPass /
SSAOGen) では、host の `PassRecordFn` が両者を区別する正当な手段がなく、debug_name の
per-frame strcmp (DoD 違反) を強いられる。**LightCullPass の実装より先に解決が必要。**

**M-2. 未消費フィールドの「偽の構成可能性」**
`rendering_path` / `max_lights` / `msaa_samples` は設定できるのに何も変えない。
apply_profile 時に一回 warn するか、ヘッダに未配線と明記すべき
(spec/feature/pipeline-profile-config.md:359 は値域検証を将来送りにしている)。

**M-3. 未実装 SHADOW/DEPTH pass の空 Begin/End**
render_pass / framebuffer が生成されるため `execute_compiled` が Begin/End + depth clear を
毎フレーム発行するが recorder は draw を出さない (`compiled_batch_recorder.cpp:21-47`)。
GPU 時間を捨てながら「動いて見える」。

**M-4. profile 切替で GPU リソースサイズが追従しない**
`GILightingSystem::set_config` は config 差し替えのみで shadow atlas 等の再確保をしない
(`gi_lighting_system.cpp:258-260`)。cascade_count / resolution を切替えても既存インスタンス
では GPU リソースが旧サイズのまま。`memory_config` も同様に切替非対応 (§2 参照)。

### Low

**L-1. `std::array<VkFramebuffer,4>` 固定上限**
swapchain image 数 > 4 で該当 image への描画が黙って落ちる (`pipeline_compiler.cpp:179`、
fb=NULL → BeginRenderPass せず `render_pass_scheduler.cpp:112-117`)。clear_values の 8 上限も
同様 (`pipeline_compiler.cpp:59`)。compile 時に超過を error 化すべき。

**L-2. 未知 attachment への寛容度が層で不一致**
compiler / `fill_clear_values` は warn なし zero-clear で続行 (`pipeline_compiler.cpp:61-65`)、
registry 側は build 失敗 (`render_pass_registry.cpp:99-103`、`framebuffer_registry.cpp:48-52`)。
同じ設定ミスの現れ方が層で変わり診断を難しくする。

**L-3. render_area の width/height 混成**
最初の非ゼロ width と非ゼロ height を独立に採用するため、サイズの異なる複数 target を
混ぜると width と height が別 attachment 由来になりうる (`pipeline_compiler.cpp:87-94`、
`framebuffer_registry.cpp:59-60`)。

**L-4. RenderPassScheduler の責務过多 + managed 経路の per-frame 文字列比較 (既知 L-5)**
managed `execute()` (custom pass CPU dispatch、`render_pass_scheduler.cpp:32-38`) は
compiled 経路導入後ほぼ vestigial だが毎フレーム `std::string ==` を含む。分離・整理の余地。

**L-5. descriptor pool を確保するが誰も使わない**
`pipeline_compiler.cpp:123-149` で pool 生成、`input_sets` は全 NULL (`:192-194`)、
bind ブロックは空 (`render_pass_scheduler.cpp:89-96`)。input_textures 配線 (Phase 4) まで
毎 compile で未使用 pool を確保している。

**L-6. `take_compiled_graph` の解放責任が型で守られていない**
scheduler は graph を解放しない契約がコメントのみ (`render_pass_scheduler.h:67-83`)。
正規回収者は `CompiledPathDriver` だが、host が scheduler を直接触ると descriptor pool リーク。

## 4. 維持すべき設計 (変えないこと)

1. **CompiledPathDriver の SRP** — compile タイミングと旧 graph 解放の単責務 + headless 統合テスト。
   Low/Mid/High 切替の recompile 経路はこの上に乗る。
2. **hot path の DoD** — `execute_compiled` + recorder は string 無し flat 走査。
3. **正直な計測方針** — GPU-driven 側の「warn + 未計測 0」。GI 系にも展開する (H-1)。
4. **編集 API とプリセットの整合** — Mid/High はプリセット定義自体が途中編集 API で構築されるため
   API とプリセットが乖離しない (`unit_pipeline_tiers_test` が構造を固定)。

## 5. 実装アクション (優先順)

| # | 対象 | 内容 | 規模感 |
|---|---|---|---|
| 1 | H-3 | `RenderBatch` へ transparency bit を追加し recorder で `filter_mask` を消費。両 pass profile での二重描画を解消 | 中 |
| 2 | M-1 | compile 時に pass 名 → uint16 pass_id を解決し `CompiledPass` へ格納 (他の名前解決と同じパターン)。pass_id キーの record callback 表を `RenderPassScheduler` に追加 | 中 |
| 3 | H-2 | `CompiledPathDriver::resize(w, h)` を追加し「attachments.resize → framebuffers.resize → recompile」を 1 API に束ねる。registry 個別 resize の直接呼び出しは非推奨化 | 小-中 |
| 4 | H-1 | GI 3-pass を GPU-driven と同じ「一回 warn + 未計測 0」へ是正。stats の見かけ値を廃止 | 小 |
| 5 | M-2 | 未配線フィールド (rendering_path / max_lights / msaa_samples) の apply_profile 時 warn + ヘッダ明記 | 小 |
| 6 | — | Forward+ の実体: light cull compute (`light_cull.comp`) + per-tile light index buffer + ライト配列管理 (`max_lights` が初めて意味を持つ)。前提: #2 | 大 |
| 7 | L-1 | framebuffer / clear_values の固定上限超過を compile error 化 | 小 |
| 8 | M-3 | 未実装 pass の Begin/End skip (または profile から SHADOW/DEPTH を recorder 実装まで外す) | 小 |

Phase 4 (既存 spec の呼称) と重なる項目は #6 と L-5 (input_textures 配線)。
本資料のアクション #1-#5 は Phase 4 本体より先に片付ける前提の足場整備。

## 6. アセット調達メモ (High ティア検証用)

実装済みモデルローダは FBX のみ (`model_data_handler.cpp:351` — `FBXImporter`。
glTF/GLB/OBJ/PMX は `detect_format()` の判定のみでロード経路未実装)。

- **ヒーローシーン**: Amazon Lumberyard Bistro (NVIDIA ORCA、CC-BY 4.0、FBX 配布) —
  多光源 (Forward+ 検証) と奥行き (DoF 検証) を 1 シーンで賄える。
- **CC0 調達**: Poly Haven (FBX/glTF/Blend、クレジット不要)。
- **PBR 正しさの定番**: Khronos glTF-Sample-Assets (DamagedHelmet 等。glTF なので Blender 変換要)。
- **補助**: Intel Sponza 2022 (CC-BY 4.0)、Sketchfab (CC0/CC-BY フィルタ)。
- 注意: Quixel Megascans は無料条件が Unreal Engine 利用に紐づくものが多く自社エンジン利用は
  ライセンス確認必須。MMD (PMX/PMD) は作者ごとの利用規約が厳しく検証用途でも非推奨。
- 運用: Blender CLI の glTF→FBX 一括変換スクリプトを tools に置く。中期では glTF ローダ実装が
  費用対効果最大 (無料アセットの大半が glTF 配布)。
