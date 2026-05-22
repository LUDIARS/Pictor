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

`ShaderKey` ヘルパー（`core/types.h`）: `shaderKey` の bit 63 を CUSTOM フラグ、bit 32-62 を `ShaderHandle` に割当て、SoA stream を増やさずカスタムシェーダ識別を運ぶ。バッチビルダの sort key（`shader_keys >> 48`）は CUSTOM object を自然にグループ化する。

**phase 2 に残した範囲**: compute stage の pipeline 生成（`shader_stages.comp` はシリアライズのみ通過）、mesh 駆動の頂点入力レイアウト確定（phase 1 の pipeline は頂点入力空＝`gl_VertexIndex` 前提）、任意 uniform / descriptor バインド、シェーダ permutation。

## 3. 方針2 — post-process を含むレンダリングパスのツール設定

### 現状
- 編集 UI（render_pipeline プラグイン Profile Editor）は `*.profile.json` の `post_process[]` を CRUD 可能。
- C++ `PostProcessDef`（`pipeline_profile.h`）は `name` / `enabled` のみ。エフェクトのパラメータが C++ に届かない。
- 実 post-process は `PostProcessPipeline` に**固定 4-pass**（extract → blur H → blur V → grade）でハードコード。`PostProcessConfig`（各エフェクト構造体）は別系統で `set_config` 経由で渡される。

### 設計
- **phase 1（実装対象）**: `PostProcessDef` を拡張（`kind` + パラメータ、`PostProcessConfig` を受け皿に活用）→ `pipeline_profile_serializer` で round-trip → `PipelineProfileDef` の post-process スタック → `PostProcessConfig` → `PostProcessPipeline::set_config` のブリッジを実装。これで既存エフェクト（Bloom / ToneMapping / Vignette / ColorGrading(LUT) / DoF）のパラメータと on/off が**設定駆動**になる。Ergo 側は `profile_schema.ts` を型付きフィールド化 + Editor UI でパラメータ編集。
- **phase 2（別途）**: 任意の新規 post-process pass 挿入（SSAO / TAA / FXAA 等）。`PostProcessPipeline` の固定 4-pass・固定ターゲット数・固定 descriptor 構造の解体が必要。

## 4. 方針3 — パイプライン/パスのタイムライン表示（実装済み 2026-05-22）

render_pipeline プラグインに 3 つ目のモード **Timeline** を追加（Scanner DAG / Timeline / Profile Editor）。
- 静的ガントチャート: topological level + レーン割当 + sub-pass ネスト + attachment ライフタイム。外部ライブラリ不使用。
- **phase 2**: GPU timestamp の WS 注入で実測タイムライン化（UI 側は配線済み、`render_pipeline` プラグイン `index.ts` の relay 追加 + Pictor 側 `VkQueryPool` timestamp が残）。

## 5. 実装順序と phase 境界

| 順 | 方針 | 状態 | リポジトリ |
|---|---|---|---|
| 1 | 方針3 タイムライン | ✅ 実装済み（`feat/render-pipeline-timeline`） | Ergo |
| 2 | 方針2 post-process 設定化（phase 1） | 設計済み・実装待ち | Pictor + Ergo |
| 3 | 方針1 カスタムシェーダ（phase 1） | 設計済み・実装待ち | Pictor + Ergo |

- 方針2 を先に行う理由: 系統A↔B 接続の最初の実例になり、方針2 で作る「シェーダ/パイプライン生成経路」を方針1 が再利用できる。
- **phase 2（系統B のハードコード解体）は全方針で別タスク**。本フェーズは「既存の選択・設定」に限定し、`PostProcessPipeline` / pipeline 生成のハードコード構造の解体は含まない。
