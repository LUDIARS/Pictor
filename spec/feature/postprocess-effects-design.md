# ポストプロセスエフェクト拡充設計 (Post-Process Effects Suite Design)

> 2026-07-12 起草。Unity (URP/HDRP)・UE5・OROCHI (YEBIS) に実装されている「代表的ポストプロセス」を
> Pictor の汎用チェーン (`PostProcessChain`) 上に揃えるための設計書。
> 関連: [`rendering-extensibility-design.md`](rendering-extensibility-design.md) §6.3、
> [`../../docs/design/postprocess_pipeline.md`](../../docs/design/postprocess_pipeline.md)、
> [`gi-bake-realtime-design.md`](gi-bake-realtime-design.md) (SSAO のライティング統合側)、`plan.md` Phase 7。

## 1. 背景・診断

ポストプロセス基盤 (host-driven `PostProcessPipeline` + 汎用 `PostProcessChain` + 途中編集 API +
JSON 配線) は実 Vulkan で完成している。エフェクトの網羅性が課題。

### 1.1 他エンジンとの対応表

| エフェクト | Unity URP | UE5 | OROCHI/YEBIS | Pictor 現状 |
|---|---|---|---|---|
| Bloom | ✅ | ✅ | ✅ (Glare) | ✅ 完了 (フルレス 9-tap) |
| Tone Mapping | ✅ | ✅ (Filmic) | ✅ | ✅ 完了 (ACES/Reinhard/Ext/Uncharted2/Linear) |
| Color Grading (LUT) | ✅ | ✅ | ✅ | ✅ 完了 (neutral LUT strip) |
| Vignette | ✅ | ✅ | ✅ | ✅ 完了 |
| Depth of Field | ✅ | ✅ | ✅ (bokeh) | 部分 (shader あり、High プリセット挿入経路) |
| **FXAA** | ✅ | (Fallback) | ✅ | **無し** |
| **SSAO** | ✅ | ✅ | — | shader (`ssao_gen.comp`) のみ、未配線 |
| **Motion Blur** | ✅ | ✅ | ✅ | **無し** |
| **Chromatic Aberration** | ✅ | ✅ | ✅ | **無し** |
| **Film Grain** | ✅ | ✅ | ✅ | **無し** |
| **Auto Exposure** | ✅ | ✅ | ✅ | `HDRConfig` にフラグのみ (未実装) |
| **TAA / TSR** | ✅ | ✅ (TSR) | — | **無し** (history buffer 未対応が障壁) |
| **SSR** | (HDRP) | ✅ | — | **無し** (法線バッファ未対応が障壁) |
| Lens Flare | (HDRP) | ✅ | ✅ | 無し (将来) |
| Volumetric Fog | (HDRP) | ✅ | — | 無し (plan.md Ultra 構想、将来) |

### 1.2 基盤側の制約 (現状)

| 制約 | 影響するエフェクト | 出典 |
|---|---|---|
| history buffer (寿命 >1 frame) 無し | TAA、Auto Exposure (適応)、Motion Blur (per-object) | `postprocess_chain.h:21` |
| 法線ターゲット無し (`__scene__` + `__depth__` のみ) | SSR、高品質 SSAO | `postprocess_chain.h:96-111` |
| velocity buffer 無し | per-object Motion Blur、TAA の動体対応 | — |
| push constant のみ (UBO 無し)、128B 上限 | 行列 2 枚を要する再投影系 | `postprocess_pipeline.cpp` |
| compute 用共通 dispatch 経路が薄い | histogram 系 Auto Exposure | `rendering-extensibility-design.md:65` |

## 2. 方針

**既存の汎用チェーン (`PostProcessPassDef` + `extra` 挿入 + 途中編集 API) を唯一の差し込み口とし、
固定構造の再ハードコードをしない。** エフェクトは 3 段階に分けて実装する:

- **phase 1 — 現行基盤で完結するもの**: FXAA / SSAO (PP 統合) / カメラ Motion Blur /
  Chromatic Aberration / Film Grain。history・法線・UBO を要求しない。
- **phase 2 — 基盤拡張を伴うもの**: history buffer 導入 → TAA / Auto Exposure。
  法線ターゲット (host 供給 or 深度から再構築) → SSR。mip-chain Bloom。DoF 仕上げ。
- **phase 3 (将来)**: Lens Flare / Volumetric Fog / per-object Motion Blur (velocity buffer)。

### 2.1 チェーン構成 (phase 1 完了時の全部盛り)

```
scene(HDR) → [dof] → [ssao_apply] → [motion_blur] →
  bloom extract → blur H → blur V →
  grade(bloom composite + tonemap + LUT + vignette + CA + grain) →
  [fxaa] → __output__
```

- 各 `[...]` は enabled 時のみ挿入 (`insert_post_process_pass` + `rebind_post_process_input`)。
  無効時のチェーンは従来 4 pass とバイト単位で同一を維持する (既存不変条件)。
- **FXAA はトーンマップ後 (LDR) に走る**。有効時は grade の出力を `pp_ldr` へ差し替え、
  `fxaa: pp_ldr → __output__` を末尾に足す。grade は輝度を alpha に書いて FXAA の
  luma 計算を省く (標準の FXAA 3.11 quality 実装)。
- **SSAO は PP 近似**: `__depth__` から view 空間位置 + 法線 (深度勾配再構築) を復元し、
  hemisphere sampling → シーン色へ乗算 (`scene * mix(1, ao, intensity)`)。直接光まで
  暗くなる近似だがカジュアル用途には十分。ライティング項別の正確な AO 適用
  (間接光のみ減衰) は GI 側 (`gi-bake-realtime-design.md` §3) の責務とする。
  1 pass 構成 (AO 計算 + 適用同時)。bilateral blur 分離は品質が要るとき phase 2。
- **Motion Blur はカメラ再投影方式**: 深度→NDC→前フレーム NDC へ
  `reproj = prevVP * inv(currVP)` の **1 枚の合成行列** (64B、push constant に収まる) で
  再投影し、速度ベクトルに沿って N サンプル。行列はホストが毎フレーム
  `PostProcessConfig::motion_blur` に詰め、`refresh_post_process_chain()` で push_data を更新。
- **CA / Film Grain は grade pass へ統合** (独立 pass を増やさない)。push constant の
  GradePC 末尾へフィールド追加。grain は時間シードを `refresh` で更新。

### 2.2 新規設定構造体 (`postprocess_effect.h`)

```cpp
struct FXAAConfig       { bool enabled=false; float edge_threshold=0.166f;
                          float edge_threshold_min=0.0833f; float subpix_quality=0.75f; };
struct SSAOPostConfig   { bool enabled=false; uint32_t sample_count=12; float radius=0.5f;
                          float bias=0.025f; float intensity=1.0f; float power=1.5f; };
struct MotionBlurConfig { bool enabled=false; float intensity=1.0f; uint32_t sample_count=8;
                          float max_velocity=0.05f;      // NDC 単位クランプ
                          float reproj_matrix[16] = {…}; // prevVP*inv(currVP)、ホストが毎フレーム更新
                          bool  matrix_valid=false; };   // 初回フレーム/カメラワープ時は false
struct ChromaticAberrationConfig { bool enabled=false; float intensity=0.5f;   // 0..1
                                   float start_radius=0.3f; };
struct FilmGrainConfig  { bool enabled=false; float intensity=0.35f; float response=0.8f;
                          float seed=0.0f; };            // refresh が毎フレーム進める
```

`PostProcessConfig` に `fxaa` / `ssao` / `motion_blur` / `chromatic_aberration` / `film_grain`
を追加。既存メンバの既定値・レイアウトは不変 (ABI 互換は問わないが挙動互換を保つ)。

### 2.3 シェーダ (`shaders/postprocess/`)

| ファイル | pass | 入力 | 備考 |
|---|---|---|---|
| `fxaa.frag` | fxaa | pp_ldr | FXAA 3.11 quality、luma は alpha から |
| `ssao_apply.frag` | ssao_apply | scene 色 + `__depth__` | 深度から位置/法線再構築、golden-angle spiral kernel |
| `motion_blur.frag` | motion_blur | 前段色 + `__depth__` | 合成再投影行列 1 枚、中心重み付き N tap |
| `color_grade.frag` (拡張) | grade | 既存 + CA/grain | GradePC 末尾に CA/grain フィールド追加、輝度を alpha 出力 |

SPIR-V コンパイルは既存のシェーダビルド経路に追加する。

### 2.4 系統A (profile JSON) との配線

`PostProcessDef` → `build_post_process_config()` ブリッジに新エフェクト分の kind/パラメータを追加し、
プロファイル JSON から on/off とパラメータを設定可能にする (phase 1 は C++ `PostProcessConfig`
直接設定を先行、JSON ブリッジは末尾タスク)。

## 3. phase 2 設計概要 (基盤拡張)

### 3.1 history buffer

`PostProcessPipeline` に「名前付き persistent ターゲット」を追加する。通常の中間ターゲットと違い
(a) フレーム間で内容を保持、(b) frames-in-flight 数だけ多重化しない (read 前 barrier で直列化)、
(c) resize で無効化フラグを立て、初回フレームは history 無しへ縮退。
予約名 `__history:<name>__` を `PostProcessPassDef::inputs` に許し、同名を `output` に書いた
pass が「今フレームの書き手」になる (ping-pong は Pipeline 側が自動でスワップ)。

### 3.2 TAA

history + カメラジッタ (Halton 2,3) + 再投影。ジッタはホストが投影行列へ適用する必要があるため
`PostProcessPipeline::current_jitter(w,h)` を公開しホスト契約とする。clamp は YCoCg AABB。
動体は velocity buffer が無い間はカメラ再投影のみ (ghosting は clamp で抑制)。

### 3.3 Auto Exposure

輝度ダウンサンプル chain (fragment、log-average) → 1x1 → history に適応値を保持 →
grade の exposure へ乗算。compute histogram 方式は共通 dispatch 経路が育ってから。

### 3.4 SSR

法線が要る。選択肢: (a) ホストが scene pass で RGBA16F の第 2 attachment に法線を書く
(`__normal__` 予約名新設、MRT 対応)、(b) 深度再構築 (面法線のみ、エッジ品質低)。
casual 用途は (b) で開始し、(a) を opt-in で用意する。ray march は screen-space DDA、
hit 失敗はフォールバック無し (黒 fade)。

## 4. 実装順序と phase 境界

| 順 | 項目 | phase | 依存 |
|---|---|---|---|
| 1 | 設定構造体 + chain builder 拡張 (5 エフェクト) | 1 | — |
| 2 | `fxaa.frag` / `ssao_apply.frag` / `motion_blur.frag` / grade 拡張 | 1 | 1 |
| 3 | headless テスト (チェーン構成・配線・push layout) | 1 | 1 |
| 4 | profile JSON ブリッジ拡張 | 1 | 1 |
| 5 | history buffer 基盤 | 2 | — |
| 6 | TAA / Auto Exposure | 2 | 5 |
| 7 | SSR (深度再構築版) / mip-chain bloom / DoF 仕上げ | 2 | — |
| 8 | Lens Flare / Volumetric Fog / velocity buffer | 3 | 5 |

## 5. phase 1 実装状況 (2026-07-12, `feat/postprocess-gi`)

| 項目 | 実体 | 状態 |
|---|---|---|
| 設定構造体 5 種 + `PostProcessConfig` 拡張 | `postprocess_effect.h` | ⬜ |
| chain builder: ssao_apply / motion_blur / fxaa 挿入 + grade CA/grain | `postprocess_chain.cpp` | ⬜ |
| シェーダ 3 本新規 + grade 拡張 | `shaders/postprocess/` | ⬜ |
| refresh: 再投影行列 / grain seed の毎フレーム更新 | `postprocess_chain.cpp` | ⬜ |
| headless テスト | `tests/unit_postprocess_chain_test.cpp` | ⬜ |
| profile JSON ブリッジ | `postprocess_config_bridge.cpp` | ⬜ |
