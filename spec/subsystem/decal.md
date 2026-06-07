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
- pipeline は blend モード毎に 3 本 (同一 shader)。同時上限 `kMaxDecals`=64

## パス位置

```
scene pass (color+depth) → DecalSystem::record (depth read-only, HDR color blend write) → post-process → HUD → present
```

## 依存 / フェーズ

`VulkanContext`、scene の depth/color (借用)、`fullscreen_quad.vert.spv` + `decal.frag.spv`。P2=fullscreen+Alpha、P3+ で Additive/Multiply + angle/depth fade + OBB box mesh + ゲーム側 TTL 統合。関連: [gi.md](gi.md) / [surface.md](surface.md)。
