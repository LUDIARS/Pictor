# GI ベイク + リアルタイム設計 (Global Illumination — Baked & Realtime)

> 2026-07-12 起草。カジュアルゲームでも自然な光の反射 (間接光バウンス) を出せる GI を、
> **ベイクとリアルタイムの両対応**で実装するための設計書。
> 関連: [`subsystem/gi.md`](subsystem/gi.md)、[`postprocess-effects-design.md`](postprocess-effects-design.md) (SSAO の PP 側)、
> `plan.md` (Ultra tier)、[`rendering-extensibility-design.md`](rendering-extensibility-design.md)。

## 1. 背景・診断

GI サブシステムは **API 表面・設定モデル・行列演算・シリアライズまで完備だが、実行が全て no-op スタブ**。

| 層 | 実体 | 状態 |
|---|---|---|
| API / 設定 (`GIConfig` / `ShadowMapConfig` / `SSAOConfig` / `GIProbeConfig` / `GIBakeConfig`) | `gi_lighting_system.h` / `gi_bake.h` | ✅ 完備 |
| CSM 行列演算 (cascade split / light view-proj / uniform 構築) | `gi_lighting_system.cpp` | ✅ 動作 |
| bake 結果の binary save/load / progress callback / `IBakeDataProvider` | `gi_bake.cpp` | ✅ 動作 |
| **bake 演算** (`bake_ao` / `bake_shadows` / `bake_irradiance` / `bake_lightmap`) | `gi_bake.cpp:217-344` | ❌ 入力を `(void)` 破棄、固定値を返すのみ |
| **probe データの GPU 転送** (`upload_probe_data`) | `gi_lighting_system.cpp:252-256` | ❌ `(void)sh_data;` |
| **GPU dispatch** (shadow 描画 / SSAO / probe 補間) | `gi_lighting_system.cpp:310-325` | ❌ コメントのみの no-op |
| shader (`shadow_map_gen.comp` / `ssao_gen.comp` / `gi_probe_sample.comp` / `lightmap_bake.comp` / `static_ao_bake.comp` / `shadow.glsl`) | `shaders/` | オーサリング済み・未 dispatch |
| マテリアル側の GI 項サンプリング | `pbr.frag` / `lit.frag` | ❌ 未対応 (ambient 定数のみ) |

さらに構造的な制約が 2 つ:

1. **`GILightingSystem` は `VkDevice` を持たない** (`GPUBufferManager` + `SceneRegistry` のみ)。
   compute pipeline / descriptor / コマンド発行の置き場が無い。
2. **メッシュ (VkBuffer) はホスト所有** (host-driven 原則)。Pictor 単独では三角形単位の
   レイトレースも shadow depth 描画もできない。Pictor が常時持つ形状情報は
   `SceneRegistry` の **オブジェクト単位のトランスフォーム + バウンディング情報**。

## 2. 方針

**「per-object GI」を正式なプロダクト方針とする。** これは妥協ではなく、既存データモデル
(`BakedAO` / `BakedIrradiance` / `BakedLightmap` が全て per-object) が最初からそう設計されている。
カジュアルゲーム (オブジェクト数中規模・スタイライズド表現) では、テクセル単位ライトマップよりも

- オブジェクト単位の AO / 間接光 SH → 頂点/ピクセルで補間
- probe grid (L2 SH、9 係数) からの動的サンプル

の組が品質/コスト比で優る。UE5 Lumen 級の per-texel GI は目標にしない。

3 phase に分ける。**phase 1 は CPU で完結し headless テスト可能** (実行禁止環境でも検証できる)。

- **phase 1 — CPU ベイク実体 + CPU リアルタイム relight**: bake 4 種の実演算 (AABB プロキシに
  対するレイキャスト)、probe grid の SH 構築、ライト変更時の probe 再計算 (リアルタイム経路)、
  `upload_probe_data` の実 GPU 転送。
- **phase 2 — GPU 配線**: `GIGpuExecutor` 新設 (compute dispatch / descriptor 所有)、
  `gi_probe_sample.comp` / `ssao_gen.comp` の実 dispatch、CSM depth の host-driven 描画契約、
  `pbr.frag` / `lit.frag` の GI 項サンプリング。
- **phase 3 (将来)**: screen-space GI、DDGI 風 probe 自動更新、reflection probe。

### 2.1 ジオメトリプロキシ — CPU ベイクの共通基盤

三角形が無いので、ベイクは **オブジェクト境界 (OBB/球) プロキシへのレイキャスト**で行う。

```cpp
/// gi/gi_scene_proxy.h — bake / relight が共有する遮蔽クエリ
class GISceneProxy {
public:
    void build(const SceneRegistry& registry);          // static pool → flat SoA
    bool occluded(const float3& from, const float3& dir,
                  float max_dist, ObjectId ignore) const; // any-hit
    float hit_distance(...) const;                       // closest-hit (バウンス用)
    // 内部: flat な center/half_extent/rotation の SoA (DoD 規約準拠)
};
```

- AABB/OBB スラブ判定のみ。数百〜数千オブジェクト × 数百レイ/obj は CPU で十分速い
  (bake は blocking API、progress callback 完備)。
- `IBakeDataProvider` に将来 `get_occluder_triangles()` を足せば精密化できる (phase 3)。

### 2.2 phase 1 の各ベイク実装 (`gi_bake.cpp` のスタブ置換)

| bake | 演算 | 出力 |
|---|---|---|
| `bake_ao` | オブジェクト中心 + 上半球 `sample_count` 方向 (cosine 重み、golden-angle) に `radius` レイ → 遮蔽率 | `BakedAO::occlusion` (0..1) |
| `bake_shadows` | 既存 CSM split/行列で cascade 割当 (`cascade_flags`)、light 方向レイで遮蔽深度 | `BakedShadow` |
| `bake_irradiance` | probe grid を先に構築 (§2.3) → オブジェクト位置で trilinear 補間した SH | `BakedIrradiance` (9×vec4) |
| `bake_lightmap` | direct: sun + `IBakeDataProvider` の point lights を遮蔽レイ付きで評価。indirect: `bounce_count` 回のプロキシ面バウンス (albedo は一律 0.5 仮定、`indirect_intensity` 係数) | `BakedLightmap` (direct RGB + indirect RGB) |

### 2.3 probe grid 構築と「リアルタイム」の定義

```cpp
/// gi/gi_probe_field.h — probe grid の SH 構築 + 補間 (bake / realtime 共用)
class GIProbeField {
public:
    void build(const GIProbeConfig&, const GISceneProxy&,
               const DirectionalLight&, const std::vector<PointLight>&);
    void relight(const DirectionalLight&, const std::vector<PointLight>&); // 遮蔽キャッシュ再利用
    const float* sh_data() const;      // 9×vec4 × probe 数 (upload_probe_data 互換)
    void sample(const float3& pos, float out_sh[36]) const; // trilinear
};
```

- **build**: probe ごとに N 方向 (既定 64、fibonacci sphere) へ遮蔽レイ → 空なら sky +
  directional、遮蔽ならバウンス面の反射光を推定 → L2 SH へ射影。
  **方向ごとの遮蔽結果 (hit 距離/ID) をキャッシュする**のが鍵。
- **relight**: ジオメトリ不変なら遮蔽キャッシュを再利用して SH 再射影のみ —
  2048 probe × 64 方向で数 ms オーダー。**これが「リアルタイム GI」の phase 1 実体**:
  ライト (太陽の色/向き、点光源) が毎フレーム変わっても probe が追従し、
  `upload_probe_data` → GPU で動的オブジェクトにも自然な間接光が乗る。
- 静的シーン + 動的ライト + 動的オブジェクト、というカジュアルゲームの典型構成を
  フルカバーする。静的ジオメトリが変わったら `invalidate()` → 再 build。

### 2.4 ベイク vs リアルタイムの役割分担

| データ | ベイク (GIBakeSystem) | リアルタイム (GILightingSystem) |
|---|---|---|
| 静的オブジェクトの AO / lightmap / SH | ✅ bake → binary 保存 → `apply()` | 使うだけ (skip 済み) |
| 動的オブジェクトの間接光 | — | probe field 補間 (phase 1: CPU relight + GPU 補間 or CPU 補間) |
| 影 | 静的: baked depth | 動的: CSM (phase 2 で実描画) |
| SSAO | 高サンプル object AO | screen-space (PP 側 / phase 2 compute) |

### 2.5 phase 2 — GPU 配線の骨子

- **`GIGpuExecutor` 新設** (`gi/gi_gpu_executor.h`): `VulkanContext` を受け、compute pipeline
  (ssao_gen / gi_probe_sample / shadow_map_gen) + descriptor を所有。`GILightingSystem::execute()`
  は executor が接続されているときだけ実 dispatch する (未接続 = 現状の CPU-only、後方互換)。
  SRP 上、Vulkan 資源管理を `GILightingSystem` に足さない。
- **CSM depth は host-driven**: `PostProcessPipeline` と同じ契約で
  `shadow_render_pass()` / `shadow_framebuffer(cascade)` を公開し、ホストが自分のメッシュを
  light view-proj で描く (`shadow_depth.vert/frag` 提供済み)。
- **マテリアル統合**: `pbr.frag` / `lit.frag` に `object_irradiance` SSBO の SH 評価
  (ambient 項置換) + `shadow.glsl` の CSM サンプル + `ao_output` 乗算を追加。
  binding 追加はホスト側 descriptor layout 変更を要するため、spec の
  `setup/integration.md` に移行手順を明記する。

## 3. SSAO の二重定義について

SSAO は (a) PP 近似 (`postprocess-effects-design.md` §2.1、シーン色乗算) と
(b) GI 統合 (`ssao_gen.comp` → `ao_output` → マテリアルが間接光のみ減衰) の 2 経路を持つ。
(a) は導入コストゼロで見た目が付く入口、(b) が最終形。両方 enabled の場合は (b) を優先し
(a) を自動 off にする (二重適用防止、config ブリッジで判定)。

## 4. 実装順序と phase 境界

| 順 | 項目 | phase | 依存 |
|---|---|---|---|
| 1 | `GISceneProxy` (AABB プロキシ + 遮蔽クエリ) | 1 | — |
| 2 | `GIProbeField` (build / relight / sample / SH 射影) | 1 | 1 |
| 3 | bake 4 種の実装置換 + stats 実値化 | 1 | 1,2 |
| 4 | `upload_probe_data` 実 GPU 転送 | 1 | — |
| 5 | headless テスト (遮蔽/SH/relight/bake round-trip) | 1 | 1-4 |
| 6 | `GIGpuExecutor` + compute dispatch 3 種 | 2 | — |
| 7 | CSM host-driven 描画契約 | 2 | 6 |
| 8 | `pbr.frag`/`lit.frag` GI 項 + integration 手順 | 2 | 6,7 |
| 9 | SSGI / DDGI 風更新 / reflection probe | 3 | 6 |

## 5. phase 1 実装状況 (2026-07-13, `feat/postprocess-gi`)

| 項目 | 実体 | 状態 |
|---|---|---|
| `GISceneProxy` | `include/pictor/gi/gi_scene_proxy.h` + `src/gi/gi_scene_proxy.cpp` (slab 法 any-hit / closest-hit、self-ignore) | ✅ |
| `GIProbeField` | `include/pictor/gi/gi_probe_field.h` + `src/gi/gi_probe_field.cpp` (遮蔽キャッシュ + relight + trilinear `sample_probe_grid`) | ✅ |
| SH L2 射影/評価ユーティリティ | `include/pictor/gi/gi_sh.h` (`sh_add_radiance` / `sh_eval_irradiance` / fibonacci sphere) | ✅ |
| bake_ao / bake_shadows / bake_irradiance / bake_lightmap 実装 | `src/gi/gi_bake.cpp` (`prepare_bake_scene` + 4 パス実演算、progress キャンセル対応) | ✅ |
| probe データ保持 + per-object 補間 | `gi_lighting_system.cpp`: `upload_probe_data` が CPU 保持、`execute_gi_probe_pass` が dynamic pool を毎フレーム trilinear 補間 (`dynamic_object_irradiance()` で公開)。**実 GPU 転送は phase 2 (`GIGpuExecutor`) へ** — `GpuAllocation` は現状アカウンティングのみで実 VkBuffer を持たないため、この層で「転送した」と偽らない | ✅ (CPU 経路) |
| headless テスト | `tests/unit_gi_probe_field_test.cpp` (SH/proxy/field/relight/include_direct) + `tests/unit_gi_bake_test.cpp` (幾何反映/決定性/round-trip/キャンセル) | ✅ |

設計判断の追記:
- **probe SH は既定で間接光 (空 + 1 次バウンス) のみ**を持つ。 直接光はマテリアル
  シェーダが解析的に評価するため、 probe に畳むと二重計上になる
  (`GIProbeField::BuildParams::include_direct` で opt-in 可)。
- `BakedShadow::depths` の意味を「静的太陽可視率 (soft shadow factor)」 と定義。
  cascade 割当はカメラ依存でありベイク時に決められないため flags は全立て。

## 6. phase 2 実装状況 (2026-07-13, `feat/postprocess-gi`)

| 項目 | 実体 | 状態 |
|---|---|---|
| `GIGpuExecutor` | `gi/gi_gpu_executor.{h,cpp}`。 実 VkBuffer (params UBO / transforms / visibility / probe SH / object irradiance、 host-visible 永続マップ) + `gi_probe_sample.comp` の compute pipeline を自己所有。 `upload_probe_sh()` / `update_objects()` / `record()` (dispatch + compute→shader barrier)。 shader 不在は即 false (fail-fast) | ✅ |
| CSM host-driven 描画契約 | `gi/gi_shadow_atlas.{h,cpp}`。 D32 array (cascade layer) + depth-only render pass (CLEAR → SHADER_READ_ONLY) + per-cascade framebuffer + PCF compare sampler。 描画はホスト責務 (`shadow_depth.vert/.frag` 提供) | ✅ |
| マテリアル GI 項 | `shaders/gi.glsl` (set 2 binding 2/3、 per-pixel probe 補間 + SH irradiance) + `pbr_main.glsl` 分割 + **`pbr_gi.frag` (opt-in バリアント — 既存 `pbr.frag` は無変更でホスト非破壊)** | ✅ |
| 移行手順 | `spec/setup/integration.md` §6 (バインディング表 + ホスト契約) | ✅ |
| SSAO compute | `gi/gi_ssao_compute.{h,cpp}` + `ssao_gen.comp` 書き換え — **法線バッファ要求を撤廃** (深度差分から 5-tap 再構築、 ノイズはシェーダ内ハッシュ)。 R8 AO image + カーネル (`generate_ssao_kernel()`、 決定的) + compute dispatch を自己所有。 depth はホスト供給 (`set_depth_input()`)。 PP 近似 `ssao_apply` の上位互換 — 両方 enabled にしない (§3) | ✅ |

## 7. phase 3 実装状況 (2026-07-13, `feat/postprocess-gi`)

| 項目 | 実体 | 状態 |
|---|---|---|
| DDGI 風 probe 自動更新 | `GIProbeField::update_budgeted()` — round-robin で毎フレーム `probe_budget` 個だけ遮蔽レイを撃ち直し + 再射影。 数フレームで全 probe が一巡し**動いたジオメトリに間接光が追従**する。 動的オブジェクトを遮蔽物に含める `GISceneProxy::build(static, dynamic)` 追加 | ✅ |
| Reflection probe | `gi/gi_reflection_probe.{h,cpp}` — mip 付き RGBA16F cubemap + face 描画契約 (host-driven、 GIShadowAtlas と同型) + blit mip 生成。 キャプチャ無しホスト用の 1x1 黒 fallback (`initialize_fallback()`) | ✅ |
| 環境スペキュラ | `gi.glsl` に set 2 binding 4 (samplerCube) + `sampleGIEnvSpecular()` (roughness → mip LOD の粗い prefilter 近似 — 本式 GGX prefilter はしない)。 `pbr_gi.frag` の ambient スペキュラ項が読む。 強度/mip は `GIGpuExecutor::set_env_params()` (UBO 64→80B 拡張、 compute 側は先頭 64B 宣言のまま互換) | ✅ |
| SSGI (screen-space GI) | postprocess 側で実装 — `postprocess-effects-design.md` §7 参照 (half-res + history 時間フィルタ)。 probe GI の補助 | ✅ |
| GILightingSystem からの一体駆動 (executor 群の自動配線) | — | ⬜ (将来 — host-driven 原則との整理が先) |

備考: `shadow.glsl` の GLSL 予約語バグ (`sample` 引数名) を修正 — pbr.frag 系を
初めてビルド対象にしたことで露見 (従来はホスト側コンパイル頼みで未検証だった)。
新たに `pbr.vert/.frag/.pbr_gi.frag/gi_probe_sample.comp/shadow_depth.*` を
`pictor_shaders` のコンパイル対象へ追加し、 CI でシェーダが検証されるようにした。
