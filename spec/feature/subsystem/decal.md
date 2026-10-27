# Decal — 投影デカール

> 実装: `include/pictor/decal/decal_system.h`, `src/decal/`、設計 `docs/design/decals.md`

scene depth から world 位置を復元し、OBB 内に収まる画素へデカールテクスチャを投影する
フルスクリーンパス。scene pass の後・post-process の前に挿入する。

## 構成

| 型 | 役割 |
|---|---|
| `DecalSystem` | デカールの追加/更新/削除 + scene HDR への投影パス記録 |
| `DecalDescriptor` | world transform (単位 OBB `[-0.5,0.5]³`→world) / VkImageView / blend / opacity / angle fade / depth fade / sort order |
| `DecalBlend` | Alpha / Additive / Multiply |
| `DecalHandle` | 1-based 32bit (0=INVALID)、free-list でスロット再利用 |
| `DecalMaterialHandle` | デカールマテリアル (フラグメントシェーダ)。0=組み込み `decal.frag`、`register_material()` が 1 以上を返す |

## 投影技法

1. 各デカールは local 単位ボックス `[-0.5,0.5]³` を `world_transform` で配置、投影軸 = local Y (column1)
2. フルスクリーン三角形 (P2) を描き、fragment で `gl_FragCoord` + scene depth + inverse view-proj から world 位置復元
3. `inverse(world_transform)` (push constant) で decal local へ → `[-0.5,0.5]³` 外は discard
4. local XZ → texture UV、angle fade (法線 vs 投影軸) + depth fade (Y 端) を適用、blend モードで scene HDR に合成

## 主要 API

```cpp
bool initialize(VulkanContext&, const std::string& shader_dir,
                VkFormat scene_color_format, VkImageView scene_color, VkImageView scene_depth, VkExtent2D);
bool resize(VkImageView scene_color, VkImageView scene_depth, VkExtent2D);
void record(VkCommandBuffer, const float view[16], const float proj[16]);  // scene 後・PP 前
DecalHandle add(const DecalDescriptor&);
void update(DecalHandle, const DecalDescriptor&);
void remove(DecalHandle); void clear();
```

- TTL は呼び出し側 (例: ゲーム側 GameRenderer) が opacity を更新して管理。本層は寿命を持たない
- descriptor set0 = scene (UBO inv-view-proj + depth sampler)、set1 = per-decal texture。push constant `DecalPC` (inv_decal[16] + opacity/angle/depth fade + axis)
- pipeline は 1 マテリアルにつき blend モード毎に 3 本 (同一 shader)。同時上限 `kMaxDecals`=64

## マテリアル (独自フラグメントシェーダ)

```cpp
DecalMaterialHandle register_material(const std::string& frag_spv_path);  // initialize 後
uint32_t            material_count() const;                               // 組み込みを含む
// DecalDescriptor::material / params[kDecalMaterialParamCount (=8)]
```

- 契約は `shaders/decal/decal_common.glsl` に集約する。set0 (UBO inv-view-proj + depth)、set1 (decal texture)、
  push constant `DecalPC` (inv_decal / opacity / angle・depth fade / axis / `params[2]` = vec4 x2、計 128 byte) は
  組み込みと独自マテリアルで共通。独自シェーダは `#include "decal_common.glsl"` して `decal_sample()` で
  OBB 判定・UV・法線・fade を得て、色だけを自分で決める。
- 登録時にブレンドモード 3 種の pipeline を作る。失敗時は `DEFAULT_DECAL_MATERIAL` を返し、デカールは組み込みで描く。
- 描画時のマテリアル選択は `resolve_decal_material()` (純関数)。未登録・範囲外のハンドルは組み込みへ寄せる。
- 時間など毎フレーム変わる値はホストが `params` に入れて `update()` する (本層は時刻を持たない)。
- 組み込み `decal.frag` は `params` を使わないので、マテリアル未指定のデカールの見た目は従来と同じ。
- 利用例: ErgoDemo の `inukare_decal` (切り絵コラージュ風マテリアル)。

## パス位置

```
scene pass (color+depth) → DecalSystem::record (depth read-only, HDR color blend write) → post-process → HUD → present
```

## 依存 / フェーズ

`VulkanContext`、scene の depth/color (借用)、`fullscreen_quad.vert.spv` + `decal.frag.spv` (+ 登録した独自マテリアルの `.frag.spv`)。P2=fullscreen+Alpha、P3+ で Additive/Multiply + angle/depth fade + OBB box mesh + ゲーム側 TTL 統合。関連: [gi.md](gi.md) / [surface.md](surface.md)。
