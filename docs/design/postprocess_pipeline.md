# ポストプロセスパイプライン設計書

## 1. 概要

Pictor のポストプロセスを「設定構造体 + 抽象クラスの足場だけ」の状態から、
**実 Vulkan の host-driven パイプライン**に置き換え済み (実装完了)。本書は
現状の設計を記録し、残っている技術的負債のクリーンアップ計画を定める。

ホストが 3D シーンを pipeline 所有の HDR ターゲットに描画し、`record()` で
エフェクトチェーンを swapchain へ直接適用する:

```
scene(HDR RGBA16F)
  → bloom 抽出 (bright-pass)
  → blur H → blur V (separable Gaussian, full-res)
  → 最終合成 (bloom composite + ToneMapping + LUT + Vignette)
  → swapchain
```

実装は 4 つのフルスクリーンパスで構成され、無効なエフェクトは push 定数で
identity 化して常に全パスを走らせる (中間イメージのレイアウトを毎フレーム
有効に保つため)。

## 2. アーキテクチャ

### 2.1 リソース所有

`PostProcessPipeline` が以下を所有する:

| リソース | 用途 |
|----------|------|
| `rp_scene_` (RGBA16F, CLEAR) | ホストがシーンを描く HDR render pass |
| `rp_inter_` (RGBA16F, DONT_CARE) | bloom 中間 ping/pong |
| `rp_output_` (swapchain fmt, DONT_CARE) | 最終出力。finalLayout は COLOR_ATTACHMENT_OPTIMAL (HUD を後段 LOAD で重ねられるよう) |
| `scene_` / `ping_` / `pong_` RenderTarget | HDR イメージ + view + framebuffer |
| `output_fbs_` | swapchain image view ごとの最終 framebuffer |
| LUT テクスチャ (R8G8B8A8_UNORM) | ホストがデコードした neutral LUT strip |
| sampler / descriptor pool / 3 pipelines | extract / blur / grade |

### 2.2 host-driven API

```cpp
bool initialize_vulkan(VulkanContext&, shader_dir, w, h, output_format,
                       output_views, config,
                       lut_rgba = nullptr, lut_w = 0, lut_h = 0);
VkRenderPass  scene_render_pass();   // ホストがシーンを描く先
VkFramebuffer scene_framebuffer();
void          record(VkCommandBuffer, output_index, dt);  // 全チェーン
void          set_config(const PostProcessConfig&);
PostProcessConfig& config_mut();     // ライブチューニング (ergo_bind)
```

`PostProcessConfig` は `HDRConfig` / `BloomConfig` / `ToneMappingConfig` /
`VignetteConfig` / `ColorGradingConfig` を束ねる。

### 2.3 設計判断

- **LUT デコードはホスト責務** — Pictor は画像デコード依存 (stb 等) を持たず、
  ホストが RGBA8 画素を渡す。
- **sRGB スワップチェーン時のガンマ** — 出力フォーマットが `*_SRGB` なら HW が
  linear→sRGB エンコードするため、シェーダ側のガンマは 1.0 に落として二重適用を防ぐ。
- **無効エフェクトも全パス実行** — `bloom.enabled=false` でも threshold を巨大値に
  して抽出 0、強度 0 で合成。中間イメージのレイアウト遷移を常に確定させる。

## 3. 技術的負債とクリーンアップ計画

### 3.1 旧フレームワークの撤去 (優先度: 高)

新パイプラインは旧 `PostProcessEffect` 基底および `BloomEffect` /
`ToneMappingEffect` / `DepthOfFieldEffect` / `GaussianBlurEffect` を
**一切使っていない**。これらは standalone でコンパイルは通るが死にコード。

- **対応**: 旧 per-effect クラス 4 つと `postprocess_effect.h` の
  `PostProcessEffect` 抽象基底、`execute(TextureHandle...)` no-op shim を削除。
  `PostProcessConfig` と各 `*Config` 構造体だけ残す。
- `bloom_effect.{h,cpp}` / `tone_mapping_effect.{h,cpp}` /
  `depth_of_field_effect.{h,cpp}` / `gaussian_blur_effect.{h,cpp}` を削除し
  CMake から外す。

### 3.2 PictorRenderer の managed パス修正 (優先度: 高)

`PictorRenderer::render()` は `postprocess_->execute(0,1,2,dt)` を呼ぶが、
これは現在 no-op。managed レンダラ経由のポストプロセスは黙って無効。

- **対応**: managed パスからポストプロセス呼び出しを削除する (host-driven が
  唯一の正規パス)。`PictorRenderer` が将来ポストプロセスを使うなら、
  `initialize_vulkan` 相当を内部で呼ぶ正規実装を別途設計する。

### 3.3 `resize()` の追加 (優先度: 中)

`initialize_vulkan` は固定 w/h。スワップチェーン再生成でターゲットが stale 化する。

- **対応**: `resize(w, h, output_views)` を追加し、render pass は再利用、
  イメージ/framebuffer のみ作り直す。

### 3.4 その他 (優先度: 低)

- `find_memory_type_` は失敗時 0 を返す → エラー伝播に変更。
- `R16G16B16A16_SFLOAT` のフォーマットサポートを `vkGetPhysicalDeviceFormatProperties`
  で確認。
- Bloom を mip チェーン方式 (progressive downsample/upsample) にすると広く柔らかい
  bloom が出せる。現状はフルレス 9-tap 固定で半径が狭い。

## 4. 実装ステータス

| 項目 | 状態 |
|------|------|
| 実 Vulkan パイプライン (4 パス) | ✅ 完了・動作検証済み |
| Vignette / Bloom / LUT / ToneMapping | ✅ 完了 |
| host-driven API | ✅ 完了 |
| 旧フレームワーク撤去 (3.1) | ⬜ 未 |
| PictorRenderer 修正 (3.2) | ⬜ 未 |
| resize 対応 (3.3) | ⬜ 未 |
| mip-chain bloom (3.4) | ⬜ 未 (将来) |
