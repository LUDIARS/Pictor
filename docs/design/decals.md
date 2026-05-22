# デカール設計書

## 1. 概要

シーンの既存サーフェス (地面・壁・キャラ) の上にテクスチャを「貼り付ける」
**投影デカール (projected decal)** の仕組みを Pictor に追加する。弾痕・
着弾痕・足跡・範囲表示・スキルマーカーなど、ジオメトリを足さずに見た目を
追加する用途。

方式は **スクリーンスペース投影デカール** — シーン深度から各ピクセルの
ワールド座標を再構成し、デカールの OBB (有向境界ボックス) 内かどうかを
判定して、ボックスのローカル UV でデカールテクスチャをサンプルし合成する。

### 1.1 前提: 深度バッファの追加

現在の host-driven シーン描画 (`WorldRenderer`) は**深度アタッチメントを
持たない** (`stage` / `skinned` レンダラは `depthTestEnable=FALSE`)。投影
デカールは深度からワールド座標を復元するため、まず深度を導入する:

- `PostProcessPipeline` の `rp_scene_` に **深度アタッチメント** (D32_SFLOAT)
  を追加。`scene_` RenderTarget に深度イメージ/ビューを持たせる。
- `StageRenderer` / `SkinnedRenderer` のパイプラインを深度テスト有効
  (`depthTestEnable=TRUE`, `depthWriteEnable=TRUE`, compareOp `LESS_OR_EQUAL`)
  に変更。両者は既に `set_scene_render_pass()` で render pass 注入済みなので、
  深度付き render pass を渡せば追従する。
- デカールパスと、必要なら半透明ソートのためにシーン深度を read する。

> 深度導入は描画の正しさ (キャラが地面に正しくめり込まない等) にも効くので、
> デカール抜きでも価値がある。

## 2. アーキテクチャ

```
┌──────────────────────────────────────────────────────────────┐
│  WorldRenderer (host)                                        │
│                                                              │
│  ① scene pass  → HDR color + depth (rp_scene_)               │
│       stage_ / skinned_ が深度書き込みありで描画             │
│                                                              │
│  ② decal pass  → HDR color に加算合成 (depth は read only)    │
│       DecalSystem.record(cmd, depth_view, scene_color_fb)    │
│       各デカール OBB をフルスクリーン or ボックスで描画       │
│                                                              │
│  ③ post-process → swapchain                                  │
│  ④ HUD pass                                                  │
└──────────────────────────────────────────────────────────────┘
```

デカールパスはシーン pass と post-process の **間** に入る。HDR color
ターゲットに直接合成する (post-process が後で tonemap する)。

### 2.1 デカールの表現

```cpp
struct DecalDescriptor {
    float        world_transform[16];  // OBB の中心・回転・サイズ (TRS)
    VkImageView  texture;              // デカール画像 (RGBA)
    DecalBlend   blend = ALPHA;        // ALPHA / ADDITIVE / MULTIPLY
    float        opacity      = 1.0f;
    float        angle_fade   = 0.35f; // 投影面の法線とデカール軸の角度フェード
    float        depth_fade   = 0.05f; // OBB の near/far 端でのフェード幅
    uint32_t     sort_order   = 0;     // 同位置の重ね順
};
```

OBB はローカル空間 `[-0.5,0.5]^3` の単位ボックスを `world_transform` で
変換したもの。デカールは OBB の -Y 軸 (または指定軸) 方向に「投影」される。

### 2.2 デカールパスのシェーディング

各デカールについて、その OBB を覆うフルスクリーン三角形 (または OBB を
実際に描いてピクセルを絞る最適化版) を描画し、フラグメントシェーダで:

1. `gl_FragCoord` + シーン深度 → クリップ空間 → ワールド座標 `P` を復元。
2. `P` を `inverse(world_transform)` でデカールローカル空間へ。
3. ローカル座標が `[-0.5,0.5]^3` の外なら `discard`。
4. ローカル XZ をデカール UV とし `texture` をサンプル。
5. シーン深度から復元した法線 (depth の ddx/ddy) とデカール投影軸の角度で
   `angle_fade`、ローカル Y で `depth_fade` を掛ける。
6. `blend` モードに従って HDR color へ合成。

push 定数: `inverse(world_transform)`、`projection_inverse`、`opacity`、
fade パラメータ、blend モード。

### 2.3 描画の最適化 (段階 2)

- 段階 1 はフルスクリーン三角形 × デカール数 (シンプル・正しい)。
- 段階 2 で OBB の実ボックスメッシュを描いてラスタライズ範囲を絞る
  (front-face cull で内側からも見える)。
- 段階 3 でクラスタ化 / デカールアトラスでパス数を削減。

## 3. API

```cpp
namespace pictor {
class DecalSystem {
public:
    bool initialize(VulkanContext&, const std::string& shader_dir,
                    VkRenderPass scene_color_pass, VkExtent2D extent);
    void shutdown();

    DecalHandle add(const DecalDescriptor&);
    void        update(DecalHandle, const DecalDescriptor&);
    void        remove(DecalHandle);
    void        clear();

    // scene pass 後、 post-process 前に呼ぶ。 depth は read-only でサンプル。
    void record(VkCommandBuffer cmd, VkImageView scene_depth, float dt);
};
}
```

デカールテクスチャは呼び出し側 (ホスト) が `GpuTexture` / `VkImageView` を
用意して渡す (Pictor は画像デコード依存を持たない方針を踏襲)。

## 4. PrivateGame 側の利用

| 用途 | デカール |
|------|----------|
| スキル範囲表示 (Shockwave/Magnet) | 地面に円形デカール、`depth_fade` 大 |
| 着弾痕・足跡 | 短命デカール、time 経過で `opacity` フェード → `remove` |
| ボスの予兆マーカー | 点滅する加算デカール |

`WorldRenderer` が `DecalSystem` を所有し、`build_frame_` で寿命管理 (TTL で
フェードアウト → remove)。

## 5. 段階実装

| 段階 | 内容 |
|------|------|
| P1 | `rp_scene_` に深度追加、stage/skinned を深度テスト有効化、深度動作確認 |
| P2 | `DecalSystem` 骨格 + フルスクリーン投影デカールパス (ALPHA blend のみ) |
| P3 | ADDITIVE/MULTIPLY、angle/depth fade、OBB ボックス描画最適化 |
| P4 | PrivateGame 統合 (スキル範囲・着弾痕)、TTL フェード |

## 6. 検討事項 / 未決

- 深度の **reverse-Z** を採用するか (精度向上だが既存射影行列の見直しが要る)。
  段階 1 では従来 Z で進め、必要なら後段で。
- スキンメッシュへのデカール投影は形状変化に追従しないため、本方式 (静的
  ワールド投影) はあくまでサーフェス向け。キャラ貼り付けは別途メッシュ
  デカール or マテリアルパラメータで。
