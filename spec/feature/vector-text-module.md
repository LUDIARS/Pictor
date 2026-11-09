# vector_text — フォントの輪郭から直接描く文字

任意モジュール `Pictor::vector_text` (`PICTOR_ENABLE_VECTOR_TEXT`、既定 ON)。
グリフを三角形メッシュにしてインスタンス描画する。ビットマップフォント、グリフアトラス、SDF アトラスを使わない。
ゲーム非依存 (PC-RULE-001)。core からこのモジュールを参照しない (PC-RULE-002)。

```cmake
target_link_libraries(app PRIVATE Pictor::vector_text)
```

## 工程

| 工程 | 実装 | 内容 |
|---|---|---|
| 輪郭取得 | `IGlyphOutlineSource` / `FontOutlineSource` | 既存の `FontLoader` + `TextSvgRenderer::extract_glyph_outline` (TrueType) |
| 直線化 | `flatten_glyph_contours` | 曲線を大きさの段階 (tier) ごとの許容誤差で分割。em 単位 (1.0 = 1 em) に正規化 |
| 塗り | `tessellate_nonzero` | 頂点と辺の交点で水平の帯に切り、nonzero で塗る区間を台形にする。同じ 2 辺の台形は上下でつなぐ |
| 縁 | `add_feather_band` | 片側だけ塗られた辺に外向きの帯を付ける。重なりに埋まった辺には付けない。交点で辺を分割してから判定 |
| キャッシュ | `GlyphMeshCache` | (font, codepoint, tier) ごとに 1 回だけ作り、平らな頂点・index 配列に追記。検索は開番地法 |
| 配置 | `layout_vector_text` | UTF-8、カーニング、改行、回転。グリフごとに `VectorTextInstance` (2x2 行列 + 平行移動 + 色) |
| まとめ | `VectorTextBatch::build_draws` | 同じメッシュのインスタンスを並べ替えて 1 回のインスタンス描画にする |
| 描画 | `VectorTextRenderer` (Vulkan) | 頂点 2 系統 (グリフ頂点 / インスタンス)。縁の帯は頂点シェーダーが画面ピクセルで押し出す |

### 大きさの段階

段階 t は 1 em が `8 * 2^t` px まで (t = 0..9、最大 4096 px)。許容誤差は段階の上限で 0.2 px。
同じ段階の中で小さく描くほど誤差は小さくなる。

### 縁の帯

帯の外側の頂点は「塗りから離れる向きの単位ベクトル」を持ち、頂点シェーダーが
`(L * offset) / sqrt(|det L|) * feather_px` だけ動かして coverage を 0 にする。
相似変換 (拡大・回転) なら帯の幅は常に `feather_px` (既定 1 px)。
`embolden_px` (擬似太字) を指定すると帯を `feather_px + embolden_px` まで押し出し、フラグメントで
被覆を `(feather + embolden) / feather` 倍して飽和させる。内側 `embolden_px` は塗り、外側 `feather_px` がぼかしになる
(SDF の dilate と同じく外側へ太る)。
角では隣り合う 2 辺の帯の間に扇を足して隙間を埋める。

## 描画の契約

```text
毎フレーム:
  batch.clear()
  layout_vector_text(cache, font, text, style, x, baseline, batch)  … 何回でも
  batch.build_draws()
  renderer.sync(cache)            … render pass の外。新しいグリフだけ転送
  (render pass 開始)
  renderer.begin_frame()
  renderer.record(cmd, extent, cache, batch)   … 1 フレームに何回でも (他の描画と交互に重ねるとき)
```

- shader は `build/shaders/vector_text/vector_text.{vert,frag}.spv`。glslc が無い場合は利用側が SPIR-V を渡す。
- インスタンスは flight 枠 (`VulkanContext::frames_in_flight()`) ごとの host-coherent バッファ。`max_instances` を超えた分は描かず、1 回だけ警告する。
- メッシュのバッファは追記のみ。容量を超えたときだけ `vkDeviceWaitIdle` して作り直す。
- `sync()` 後に作ったメッシュはそのフレームでは描かれない (次フレームで出る)。
- 混合はアルファ混合。縁の帯が塗りの上に重なっても同じ色なので見た目は変わらない。

## 境界

- 例外: 輪郭が不正 (非有限、em 無し、点数過多) なら `std::invalid_argument`。
- `FontOutlineSource` は TrueType 輪郭のみ (CFF は既存 decoder と同じく非対応)。
- 3D 空間への配置は host が 2x2 + 平行移動へ射影して渡す (このモジュールは画面空間)。
- Web / モバイルの backend は未実装。CPU 側 (メッシュ・キャッシュ・配置) は backend 非依存で、そのまま使える。

## テスト

`tests/unit_vector_text_test.cpp` (フォントファイル不要の合成輪郭):
面積 (正方形・穴・重なり・時計回り・蝶ネクタイ・円)、縁の向きと重なりでの省略、不正入力、
段階、キャッシュ (1 回だけ作る・rehash 後も同じ id)、配置 (カーニング・改行・回転)、UTF-8。

`tests/unit_vector_text_font_test.cpp` (同梱の `fonts/default.ttf`): 印字可能な ASCII 全グリフを作り、
メッシュの塗り面積が別実装の `GlyphVectorRasterizer` の被覆合計と 2% + 8 px² 以内で一致することを確かめる (128 px)。
