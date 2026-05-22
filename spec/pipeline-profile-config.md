# Pipeline Profile External Configuration (系統A)

正本スキーマ — 後続の Ergo-web パイプライン編集ツールはこのドキュメントに準拠する。

- **対象構造体**: `pictor::PipelineProfileDef` (`include/pictor/pipeline/pipeline_profile.h`)
- **シリアライザ**: `include/pictor/pipeline/pipeline_profile_serializer.h` / `src/pipeline/pipeline_profile_serializer.cpp`
- **プリセットローダ**: `include/pictor/pipeline/pipeline_profile_loader.h` / `src/pipeline/pipeline_profile_loader.cpp`
- **スキーマバージョン**: 1
- **正本サンプル**: `profiles/*.profile.json` (5 プリセット)

---

## 1. 重要: スコープ (誇張しない)

Pictor のレンダリングパイプラインは **2 系統** に分かれている。

| 系統 | 何 | このコンフィグとの関係 |
|------|----|----------------------|
| **系統A** `PipelineProfileDef` | pass 列・post_process・shadow/GI/memory 等を持つ宣言的プロファイル構造体 | **本コンフィグの対象。** JSON で完全に表現・差し替え可能 |
| **系統B** 実 `VkRenderPass` チェーン | `src/surface/vulkan_context.cpp` の `create_render_pass`、`src/postprocess/postprocess_pipeline.cpp` | **本コンフィグの対象外。** 完全ハードコード |

`PipelineProfileDef` (系統A) は現状 **系統B (実描画) と完全には接続されていない**。
`RenderPassScheduler::execute()` (`src/pipeline/render_pass_scheduler.cpp`) は
pass 列を反復し pass 種別ごとに分岐するが、各 case の中身は大半がコメントの
スタブで、実 Vulkan コマンドの記録は行わない。

### 1.1 この JSON が「現状」制御するもの

JSON を変更すると即座に C++ ランタイム状態へ反映されるもの:

- **`PipelineProfileManager` の登録内容** — どのプロファイルが存在し、各々が
  どんな pass 列・設定を持つか。`profile_names()` / `get_profile()` に反映。
- **`RenderPassScheduler` の pass 順序** (`pass_order_`) — `reconfigure()` 経由。
  `execute()` はこの順序で pass 種別ごとの分岐を回す (中身はスタブだが順序・
  種別・custom pass dispatch は実際に駆動される)。
- **`MemorySubsystem`** に渡る `MemoryConfig` — frame allocator サイズ・flight 数・
  GPU プールサイズ。プロファイル切替時 `apply_profile()` で
  `update_scheduler_->set_config()` と GI/GPU-driven の再構成に使われる。
- **`UpdateScheduler` の `UpdateConfig`** — `apply_profile()` が即時適用。
  chunk_size / NT-store 設定が CPU 並列更新の実挙動を変える。
- **`GPUDrivenPipeline` の `GPUDrivenConfig`** — `gpu_driven_enabled` の on/off で
  パイプラインの生成/破棄、`set_config()` で閾値更新。
- **`GILightingSystem` の `GIConfig`** — shadow/SSAO/probe の有効・無効と各パラメータ。
  `apply_profile()` が `set_config()` で適用、無効化時は破棄。
- **`ProfilerConfig`** — オーバーレイ表示モード・クエリ数。
- **custom pass の dispatch** — `render_passes[]` の `pass_name` が
  `ICustomRenderPass::name()` と一致すれば、その custom pass の `execute()` が
  実際に呼ばれる (これは現状でも実動作する)。

### 1.2 この JSON が制御「しない」もの (系統B でハードコードのまま)

- **実 `VkRenderPass` の attachment 構成・サブパス・ロード/ストア op** —
  `vulkan_context.cpp` でハードコード。`render_passes[].render_targets` /
  `input_textures` は構造体に格納されるが、実 framebuffer 構成には未配線。
- **post-process の実シェーダチェーン (固定 4-pass 構造)** —
  `postprocess_pipeline.cpp` の extract → blur H → blur V → grade の
  4-pass 構造・固定ターゲット数・固定 descriptor はハードコードのまま。
  `post_process[]` に**新規 pass を挿入**しても (SSAO / TAA 等) 実 GPU
  パスは増えない (= 系統B phase 2)。
  一方、既存エフェクト (Bloom / ToneMapping / Vignette / ColorGrading /
  DoF) の **パラメータと on/off** は `PostProcessDef` 拡張 +
  `build_post_process_config()` ブリッジ経由で実描画に届く (§3.4、
  方針2 phase 1 で実装済み)。
- **各 pass の実ドローコール記録** — `RenderPassScheduler::execute()` の
  `PassType::OPAQUE` / `TRANSPARENT` / `SHADOW` / `DEPTH_ONLY` / `POST_PROCESS` /
  `COMPUTE` の各 case はコメントスタブ。pass の有効/無効・順序は反映されるが、
  「pass を消す = 実描画から消える」とまではならない。
- **`shader_override`** — `RenderPassDef` に格納されるが scheduler が消費しない。

> **要約**: このコンフィグは「系統A のプロファイル定義データを完全に外部化」する。
> プロファイル選択・pass 列・メモリ/更新/GI/GPU-driven/プロファイラの設定値は
> 実ランタイムに効く。post-process の既存エフェクトのパラメータ・on/off も
> `build_post_process_config()` ブリッジ経由で実描画に効く (§3.4)。実 Vulkan
> の描画トポロジ (attachment 構成 / 実シェーダチェーンの pass 増減 / ドロー
> 記録) は系統B のままで、その可変化は別タスク。

---

## 2. ファイル形式

UTF-8 テキスト (BOM 許容)。標準 JSON。コメント不可。
未知のキーは黙ってスキップされる (前方互換)。構文エラーのみ読み込み失敗。

ファイル命名: プリセットは `<lowercased-name>.profile.json`
(例 `standard.profile.json`)。`pipeline_profile_loader` がこの規約で探索する。

---

## 3. スキーマ (version 1)

トップレベルは 1 つの JSON オブジェクト。全フィールド任意 (欠落時は
`PipelineProfileDef` の C++ 既定値、preset シード時は preset の値)。

### 3.1 プロファイルスカラ

| キー | 型 | 既定 | 説明 |
|------|----|----|------|
| `version` | int | — | スキーマバージョン。現状 `1`。読み込み時は無視 (前方互換) |
| `profile_name` | string | `""` | プロファイル名。マネージャのキー。**実用上は必須** |
| `rendering_path` | enum string | `FORWARD_PLUS` | `FORWARD` / `FORWARD_PLUS` / `DEFERRED` / `HYBRID` |
| `max_lights` | uint | `256` | ライト上限 |
| `msaa_samples` | uint | `0` | `0` / `2` / `4` / `8` |
| `gpu_driven_enabled` | bool | `true` | GPU 駆動パイプラインの有効化 |
| `compute_update_enabled` | bool | `true` | Compute Update の有効化 |

### 3.2 `render_passes` (配列)

`RenderPassDef` の配列。**存在すれば preset の pass 列を完全に置換**する。

| キー | 型 | 既定 | 説明 |
|------|----|----|------|
| `pass_name` | string | `""` | pass 名。`ICustomRenderPass::name()` と一致すれば custom pass dispatch |
| `pass_type` | enum string | `OPAQUE` | `DEPTH_ONLY` / `OPAQUE` / `TRANSPARENT` / `SHADOW` / `POST_PROCESS` / `COMPUTE` / `CUSTOM` |
| `shader_override` | string | `"none"` | `"none"` または `"handle:<u32>"`。**現状 scheduler 未消費** |
| `render_targets` | string[] | `[]` | 出力ターゲット名。**現状 framebuffer 未配線** |
| `input_textures` | string[] | `[]` | 入力テクスチャ名。**現状 未配線** |
| `sort_mode` | enum string | `FRONT_TO_BACK` | `FRONT_TO_BACK` / `BACK_TO_FRONT` / `NONE` |
| `filter_mask` | uint | `65535` | バッチフィルタビットマスク |
| `gpu_driven_pass` | bool | `false` | GPU 駆動 compute pass か |
| `required_streams` | string[] | `[]` | SoA ストリームのプリフェッチヒント |

### 3.3 `post_process` (配列)

`PostProcessDef` の配列。**存在すれば preset の post-process スタックを完全に置換**。

| キー | 型 | 既定 | 説明 |
|------|----|----|------|
| `name` | string | `""` | エフェクト名 (`Bloom` / `SSAO` / `Tonemapping` / `TAA` / `FXAA` / `VolumetricFog` 等) |
| `enabled` | bool | `true` | 有効/無効 |
| `kind` | enum string | (name から推論) | エフェクト種別。§3.4 参照 |
| `bloom` / `tone_mapping` / `vignette` / `color_grading` / `depth_of_field` | object | — | `kind` に対応するエフェクトのパラメータ。§3.4 参照 |

### 3.4 post-process のエフェクトパラメータ (実装済み — 方針2 phase 1)

`PostProcessDef` は `kind` (種別タグ) + エフェクトごとのパラメータ構造体を
持つ。`kind` で選ばれた構造体だけが意味を持ち、`build_post_process_config()`
(`include/pictor/postprocess/postprocess_config_bridge.h`) が
`post_process[]` スタックを `PostProcessConfig` に畳み込んで、実描画の
`PostProcessPipeline::set_config` へ渡せるようにする。これで Bloom /
ToneMapping / Vignette / ColorGrading(LUT) / DoF の **パラメータと on/off が
設定駆動**になる (設計書 `rendering-extensibility-design.md` §3)。

**`kind`** — 次のいずれか:
`Bloom` / `ToneMapping` / `Vignette` / `ColorGrading` / `DepthOfField` /
`Unknown`。省略時は `name` から推論される (大文字小文字不問、
`Tonemap` / `LUT` / `Grade` / `DoF` 等の別名も受理)。`SSAO` / `TAA` /
`FXAA` / `VolumetricFog` 等、host-driven な `PostProcessPipeline` に実装の
無いエフェクトは `Unknown` に解決され、ブリッジは黙って無視する (固定
4-pass 構造の解体 = 系統B phase 2 の領分)。

**エフェクトパラメータブロック** — `kind` に一致するキーだけが消費される
(他は前方互換のため黙ってスキップ)。各ブロックのフィールドは
`include/pictor/postprocess/postprocess_effect.h` の構造体に対応する:

- `bloom`: `threshold`, `soft_threshold`, `intensity`, `radius`,
  `mip_levels` (uint), `scatter` (float)。
- `tone_mapping`: `op` (enum: `ACES_FILMIC` / `REINHARD` / `REINHARD_EXT` /
  `UNCHARTED2` / `LINEAR_CLAMP`), `exposure`, `gamma`, `white_point`,
  `saturation`。
- `vignette`: `intensity`, `radius`, `softness`, `color` ([r,g,b] float 配列)。
- `color_grading`: `lut_path` (string), `lut_intensity`, `lut_size` (uint)。
- `depth_of_field`: `focus_distance`, `focus_range`, `bokeh_radius`,
  `near_start`, `near_end`, `far_start`, `far_end`, `sample_count` (uint)。

`enabled=false` のエフェクトもパラメータは保持・round-trip される
(ブリッジは `.enabled=false` を転写するだけ)。スタックに現れない
エフェクトはブリッジで `enabled=false` に倒される。

ブリッジが扱わない `HDRConfig` / `GaussianBlurConfig` は `PostProcessDef`
表現を持たない。`build_post_process_config(stack, base)` の `base` 引数で
ホスト側の HDR / blur 設定を持ち越せる。

### 3.5 `shadow` (オブジェクト) — `ShadowConfig`

| キー | 型 | 既定 |
|------|----|----|
| `cascade_count` | uint | `3` |
| `resolution` | uint | `2048` |
| `filter_mode` | enum string | `PCF` (`NONE` / `PCF` / `PCSS`) |

### 3.6 `gi` (オブジェクト) — `GIConfig`

| キー | 型 | 既定 |
|------|----|----|
| `shadow_enabled` | bool | `true` |
| `ssao_enabled` | bool | `true` |
| `gi_probes_enabled` | bool | `false` |
| `shadow` | object | `ShadowMapConfig` (下記) |
| `ssao` | object | `SSAOConfig` (下記) |
| `probes` | object | `GIProbeConfig` (下記) |

`gi.shadow` — `ShadowMapConfig`:
`cascade_count` (uint), `resolution` (uint), `depth_bias` (float),
`normal_bias` (float), `slope_scale_bias` (float), `cascade_lambda` (float),
`max_shadow_dist` (float), `cascade_blend_width` (float),
`filter_mode` (enum), `shadow_strength` (float), `pcss_light_size` (float),
`pcss_min_penumbra` (float), `pcss_max_penumbra` (float),
`pcss_blocker_search_radius` (float)。

`gi.ssao` — `SSAOConfig`:
`sample_count` (uint), `radius` (float), `bias` (float), `intensity` (float),
`falloff_start` (float), `falloff_end` (float), `blur_enabled` (bool)。

`gi.probes` — `GIProbeConfig`:
`grid_origin` ([x,y,z] float 配列), `grid_spacing` ([x,y,z] float 配列),
`grid_x` / `grid_y` / `grid_z` (uint), `gi_intensity` (float),
`max_probe_distance` (float)。

### 3.7 `memory` (オブジェクト) — `MemoryConfig`

サイズは全て **バイト単位の整数**。

| キー | 型 | 既定 |
|------|----|----|
| `frame_allocator_size` | uint (bytes) | `16777216` |
| `flight_count` | uint | `3` |
| `pool_chunk_size` | uint (bytes) | `65536` |
| `use_large_pages` | bool | `false` |
| `gpu` | object | `GpuMemoryAllocator::Config` (下記) |

`memory.gpu` — `GpuMemoryAllocator::Config` (全て bytes):
`mesh_pool_size`, `ssbo_pool_size`, `instance_buffer_size`,
`indirect_buffer_size`, `staging_buffer_size`。

### 3.8 `gpu_driven` (オブジェクト) — `GPUDrivenConfig`

| キー | 型 | 既定 |
|------|----|----|
| `max_triangle_count` | uint | `50000` |
| `min_instance_count` | uint | `32` |
| `workgroup_size` | uint | `256` |
| `two_phase_culling` | bool | `true` |
| `compute_update` | bool | `true` |

### 3.9 `update` (オブジェクト) — `UpdateConfig`

| キー | 型 | 既定 |
|------|----|----|
| `chunk_size` | uint | `16384` |
| `worker_threads` | uint | `0` (0 = 自動) |
| `nt_store_enabled` | bool | `true` |
| `nt_store_threshold` | uint | `10000` |

### 3.10 `profiler` (オブジェクト) — `ProfilerConfig`

| キー | 型 | 既定 |
|------|----|----|
| `enabled` | bool | `true` |
| `overlay_mode` | enum string | `STANDARD` (`OFF` / `MINIMAL` / `STANDARD` / `DETAILED` / `TIMELINE`) |
| `max_queries` | uint | `64` |

---

## 4. C++ API

```cpp
#include "pictor/pipeline/pipeline_profile_serializer.h"

// JSON 文字列 <-> PipelineProfileDef
std::string         to_pipeline_profile_json(const PipelineProfileDef&);
bool                from_pipeline_profile_json(const std::string& json,
                                               PipelineProfileDef& out,
                                               std::string* error = nullptr);
// preset シード版 (JSON は差分のみ記述すればよい)
bool                from_pipeline_profile_json(const std::string& json,
                                               const PipelineProfileDef& preset,
                                               PipelineProfileDef& out,
                                               std::string* error = nullptr);
// ファイル I/O
bool load_pipeline_profile_file(const std::string& path, PipelineProfileDef& out,
                                std::string* error = nullptr);
bool load_pipeline_profile_file(const std::string& path,
                                const PipelineProfileDef& preset,
                                PipelineProfileDef& out, std::string* error = nullptr);
bool save_pipeline_profile_file(const std::string& path,
                                const PipelineProfileDef& def,
                                std::string* error = nullptr);
```

```cpp
#include "pictor/pipeline/pipeline_profile_loader.h"

// 5 プリセットを profiles/ から読み込み (失敗時は C++ ファクトリにフォールバック)
PipelineProfileDef  load_builtin_preset(name, profile_dir, PresetLoadResult* = nullptr);
std::vector<...>    load_builtin_presets(profile_dir, std::vector<PresetLoadResult>* = nullptr);
void                register_presets_from_dir(PipelineProfileManager&, profile_dir, ...);
```

`PipelineProfileBuilder` (`pipeline_builder.h`) の構造化シーム:

```cpp
// 全フィールド指定の RenderPassDef を文字列トークンから組む
RenderPassDef PipelineProfileBuilder::make_pass(
    pass_name, pass_type_str, sort_mode_str = "FRONT_TO_BACK",
    render_targets = {}, input_textures = {}, required_streams = {},
    filter_mask = 0xFFFF, gpu_driven_pass = false, shader_override = "none");
PipelineProfileBuilder& add_pass_tokens(...);  // 同上を末尾追加
```

ランタイム再設定 (`pictor_renderer.h`):

```cpp
// JSON ファイルからプロファイルを読み込み、登録 + アクティブ化 + 全サブシステム再構成
bool PictorRenderer::load_profile_from_file(const std::string& path,
                                            std::string* error = nullptr);
// アクティブプロファイルを再適用 (apply_profile を再走 = scheduler reconfigure 等)
void PictorRenderer::reload_active_profile();
```

`load_profile_from_file()` / `reload_active_profile()` は内部で既存の
`apply_profile()` を呼ぶ。`apply_profile()` は `RenderPassScheduler::reconfigure()`、
`UpdateScheduler::set_config()`、GI / GPU-driven の再構成、バッチ無効化を行う。

post-process ブリッジ (`postprocess_config_bridge.h`):

```cpp
#include "pictor/postprocess/postprocess_config_bridge.h"

// post_process スタックを PostProcessConfig に畳み込む。
// `base` で HDR / gaussian_blur (PostProcessDef 表現なし) を持ち越す。
PostProcessConfig build_post_process_config(
    const std::vector<PostProcessDef>& stack, const PostProcessConfig& base = {});
PostProcessConfig build_post_process_config(
    const PipelineProfileDef& profile, const PostProcessConfig& base = {});
```

`PostProcessPipeline` は host-driven で `PictorRenderer` は所有しない。
ホストはプロファイルの `post_process_stack` を `build_post_process_config()`
に渡し、結果を `PostProcessPipeline::set_config()` (または
`initialize_vulkan` の `config` 引数) へ渡すことで、プロファイルの
post-process 設定を実描画に反映する。

---

## 5. プリセット外部化

`PipelineProfileManager::create_*_profile()` の C++ ハードコードプリセットは
`profiles/*.profile.json` に切り出した。`pipeline_profile_loader` が起動時に
読み込み、ファイル欠落・パース失敗時は **per-preset で C++ ファクトリへ
フォールバック**する (`PresetLoadResult::loaded_from_file` で判別可能)。

C++ ファクトリ (`create_lite_profile()` 等) は **削除していない**。
`pipeline_profile_loader` のフォールバック先として残置。`register_defaults()`
も従来どおり C++ ファクトリ直結のまま (後方互換)。データ駆動を使いたい
ホストは `register_presets_from_dir()` を呼ぶ。

---

## 6. 既知の制約 / 要判断

- **`shader_override` / `render_targets` / `input_textures`** — `RenderPassDef`
  に格納されるが `RenderPassScheduler` が消費しない。系統B 配線時に意味を持つ。
- **post-process パラメータ** — §3.4。`PostProcessDef` 拡張 + ブリッジは
  方針2 phase 1 で実装済み。残るのは任意 pass 挿入 (固定 4-pass の解体 =
  系統B phase 2)。
- **enum の前方互換** — 未知の enum 文字列は preset/既定値にフォールバックする
  (構文エラーにはしない)。新しい `PassType` 等を追加したら本ドキュメントを更新。
- **検証は最小限** — シリアライザは「構文が JSON か」しか見ない。
  `msaa_samples: 3` のような無意味値も格納される。値域検証はホスト側 / 系統B 配線時。
