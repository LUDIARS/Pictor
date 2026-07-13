# GI — グローバルイルミネーション (影 / AO / プローブ)

> 実装: `include/pictor/gi/` (5), `src/gi/`
> 設計: [`../gi-bake-realtime-design.md`](../gi-bake-realtime-design.md) (phase 分割と実装状況の正本)

影 (CSM) + 環境遮蔽 (SSAO) + 間接光 (irradiance probe) のランタイム + オフライン bake。
**phase 1 (CPU 経路) 実装済み** — bake 実演算と probe grid の CPU relight / 補間が動く。
GPU dispatch (compute / CSM 実描画) は phase 2 (`GIGpuExecutor`) の領分で未実装。

## 構成

| クラス | 役割 |
|---|---|
| `GILightingSystem` | ランタイム GI パス統括。可視カリング後に shadow / SSAO / probe sampling の 3 前パスを実行、cascade uniform と GPU binding を管理。probe SH の CPU 保持と dynamic pool の per-object 補間 (`dynamic_object_irradiance()`) |
| `GIBakeSystem` | Static プール向けの高品質オフライン事前計算 (CPU レイキャスト実演算) → binary 永続化 |
| `GISceneProxy` | AABB プロキシへの遮蔽クエリ (slab 法 any-hit / closest-hit)。bake / probe 構築の共通基盤 |
| `GIProbeField` | probe grid の SH 構築 + 遮蔽キャッシュ + `relight()` (動的ライト追従 = phase 1 のリアルタイム GI) + trilinear 補間 |
| `gi_sh.h` | SH L2 射影 / irradiance 評価 / fibonacci sphere (純関数) |
| `GIGpuExecutor` | phase 2: 実 VkBuffer + `gi_probe_sample.comp` compute dispatch。マテリアル結線用バッファ (`params_buffer()` / `probe_sh_buffer()` / `object_irradiance_buffer()`) を公開 |
| `GIShadowAtlas` | phase 2: CSM depth array + render pass + per-cascade framebuffer + PCF compare sampler (描画はホスト責務、`spec/setup/integration.md` §6.2) |

シェーダ側: `gi.glsl` (per-pixel probe 補間) を `pbr_gi.frag` (opt-in バリアント) が include。既存 `pbr.frag` は無変更。

設定: `GIConfig` (shadows+SSAO+probes 統合) / `ShadowMapConfig` (cascade 数/解像度/PCF・PCSS/bias) / `SSAOConfig` / `GIProbeConfig` (grid 原点/間隔/寸法)。bake は `GIBakeConfig` + `BakeTarget` enum (SHADOW_MAP / AMBIENT_OCCLUSION / PROBE_IRRADIANCE / LIGHTMAP)。

## 技法

- **影**: Cascaded Shadow Maps (1-4 cascade、既定 2048²、depth array)。hard/PCF/PCSS フィルタ、cascade 割当は `shadow_map_gen.comp`、constant+normal+slope bias
- **AO**: ランタイム SSAO (hemisphere sampling + bilateral blur)。bake は object-space 高サンプル (256 vs 32)
- **間接光**: irradiance probe grid。per-probe を **L2 球面調和 (9係数)** で保持、`gi_probe_sample.comp` でオブジェクト位置に補間。bake は per-object SH をキャッシュ
- **lightmap**: direct+indirect (multi-bounce 最大3)

## 主要 API

```cpp
// GILightingSystem
void initialize(uint32_t max_objects, uint32_t w, uint32_t h);
void execute(const float4x4& view, const float4x4& proj);   // compute cull 後
void set_directional_light(const DirectionalLight&);
void upload_probe_data(const float* sh, uint32_t probe_count);
const ShadowUniformData& shadow_uniforms() const;
// GIBakeSystem
GIBakeResult bake(BakeProgressCallback);
void apply(const GIBakeResult&);
bool save(const std::string&, const GIBakeResult&);
GIBakeResult load(const std::string&);
```

baked データ: `BakedShadow` (cascade flags + 4 depth) / `BakedAO` / `BakedIrradiance` (9×vec4 SH) / `BakedLightmap`。

## 統合 / 依存

`execute()` を compute cull 後に呼び、shadow → SSAO → probe の順に dispatch。fragment shader が `shadow_atlas` / `ao_output` / `object_irradiance` を read-only で sample。bake は `static_pool()` を 4 パス処理し binary 永続化、ランタイムは先頭 `baked_static_count()` 個を bake 済データで skip。依存: `gpu/gpu_buffer_manager.h`、`scene/`、`shaders/shadow.glsl`。関連: [decal.md](decal.md) / [gpu.md](gpu.md)。
