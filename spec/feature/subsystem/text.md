# Text — フォント読込 + グリフ描画

> 実装: `include/pictor/text/`, `src/text/`
> 外部ライブラリ非依存 (FreeType / stb を使わず TrueType/OpenType を自前パース)。CPU 側処理で、出力は texture/vertex として描画パイプラインに渡す。

## 構成

| クラス / namespace | ヘッダ | 役割 |
|---|---|---|
| `FontLoader` | font_loader.h | TTF/OTF を file/memory から読込、テーブル (head/cmap/hmtx/glyf/CFF) パース、グリフメトリクス・コードポイント対応・カーニング照会 |
| `TextRasterizer` | text_rasterizer.h | グリフをマルチページのテクスチャアトラスへラスタライズ (shelf-pack)、描画用 quad 頂点 (`TextVertex`) 生成 |
| `TextImageRenderer` | text_image_renderer.h | 文字列を RGBA `ImageBuffer` へラスタライズ (固定/自動サイズ、行折返し、整列、色合成) + 寸法計測 |
| `TextSvgRenderer` | text_svg_renderer.h | グリフ輪郭を SVG path として抽出、文字列を SVG ドキュメントへ |
| `glyph_path_effects` | glyph_path_effects.h | `GlyphOutline` (パス) への効果: stroke / drop shadow (平行移動) / glow (拡大) / polyline 化 / bbox |
| `text_effects` | text_effects.h | ラスタ済 alpha bitmap への効果: outline 膨張+tint / shadow+blur / glow + 合成ヘルパ |

## 3 つの描画経路

出力は用途別に3経路を持つ。`FontLoader` とフォント単位の `GlyphOutline`
を共通の土台とし、画像とatlasは同じベクタの被覆計算を使用する。

| 経路 | 解像度 | 用途 | 出力 |
|---|---|---|---|
| **A. TextRasterizer** | 固定 (atlas 焼込時のサイズ) | 高頻度・繰返しの多い UI/HUD | atlas `ImageBuffer[]` (page) + `TextVertex[]` (glyph 6 頂点) |
| **B. TextImageRenderer** | 動的 (毎回ラスタ) | 可変サイズ・効果付き・オフライン | RGBA `ImageBuffer` 1 枚 |
| **C. TextSvgRenderer** | 解像度非依存 (ベクター) | スケーラブル / SVG エクスポート / 外部ラスタライザ供給 | SVG XML 文字列 |

効果は経路に対応: パス系効果 (`glyph_path_effects`) は B/C 用、ラスタ系効果 (`text_effects`) は B の出力 `ImageBuffer` に適用。

## 主要 API (抜粋)

```cpp
// FontLoader
FontHandle load_from_file(const std::string& path);
bool get_glyph_metrics(FontHandle, uint32_t codepoint, float size, GlyphMetrics& out) const;
int16_t get_kerning(FontHandle, uint32_t left, uint32_t right) const;

// TextRasterizer
bool build_atlas(FontHandle, CharSet, const Config&);          // shelf-pack して page を焼く
const ImageBuffer* get_page(uint32_t page_index) const;        // GPU upload 元
std::vector<TextVertex> generate_vertices(const std::string&, const TextStyle&, float x, float y) const;

// TextImageRenderer
ImageBuffer render_text(FontHandle, const std::string&, const TextStyle&);
TextExtent  measure_text(FontHandle, const std::string&, const TextStyle&) const;

// TextSvgRenderer
GlyphOutline extract_glyph_outline(FontHandle, uint32_t codepoint) const;
std::string  render_text_svg(FontHandle, const std::string&, const TextStyle&) const;
```

## コアデータ型 (text_types.h)

- `FontHandle = uint32_t` (`INVALID_FONT`)、`GlyphMetrics` / `FontMetrics` / `GlyphAtlasEntry` (UV + page)
- `CharSet` (bitflags: ASCII / LATIN_EXTENDED / CJK_COMMON / HIRAGANA / KATAKANA / … と合成 `JAPANESE` / `KOREAN` / `WESTERN`)、`CodepointRange`
- `TextStyle` (font_size / color / align_h/v / line_spacing / letter_spacing / max_width / word_wrap)
- `GlyphOutline` (`SvgPathPoint[]`: MOVE/LINE/QUAD/CUBIC/CLOSE)、`ImageBuffer` (CPU 側 pixels + w/h/channels)、`TextVertex` (pos+uv+color)

## パイプライン統合

text は専用 render pass を持たない。出力は texture/quad として既存の Material + Mesh 経路に乗る:

```
TextImageRenderer::render_text() → ImageBuffer(RGBA) ─┐
TextRasterizer::get_page()       → ImageBuffer(page) ─┴→ GPU テクスチャ化 → Material 参照 → quad 描画
TextRasterizer::generate_vertices() → TextVertex[] → VB へ
TextSvgRenderer は SVG 文字列を返すのみ (外部ラスタライザ or Rive へ供給)
```

## 注意

- フォントパースは自前 (big-endian、cmap format 4/12)。外部フォントライブラリに差し替えない。
- アトラスは焼込時サイズ固定 → 動的スケールは B 経路 or atlas 再構築。

## SPEC-PC-VECTOR-TEXT

価値: `PC-TEXT-VECTOR-001` — UI、拡大表示、画像出力、SVG出力で同じ字形を再利用する。
ドメイン: `text-rendering`（多様な描画機能の文字サブシステム）。
既存Pf仕様: `PC-FEAT-DRAW-10` / version 2 / draft
（project `01M2438A4KK1NCBH46A0H08ANE`, spec `01M2438BSXQVH7CRR0V1VHYWQ3`）。
2026-09-28に照合した登録は旧mainの実装一覧であり、本修正の反映・動作保証ではない。

| 実装 | 責務 |
|---|---|
| `src/text/truetype_outline.cpp` | テーブル境界を検証し、単純/複合グリフを曲線のベクタ輪郭へ復元 |
| `src/text/glyph_vector_rasterizer.cpp` | ベクタ輪郭を出力解像度で分割し、nonzero windingの画素被覆を計算 |
| `src/text/font_glyph_raster.cpp` | フォント輪郭と画像bounds/bearingを対応させ、共通描画へ渡す |
| `src/text/text_svg_renderer.cpp` | 共通輪郭の公開・SVG出力 |
| `src/text/text_image_renderer.cpp` | 共通のグリフ画像を文字列へ配置・合成 |
| `src/text/text_rasterizer.cpp` | 共通のグリフ画像をatlasへ格納 |

- `TextSvgRenderer::extract_glyph_outline()` は解像度に依存しないy-upの
  フォント座標と二次曲線を返す。TrueTypeの連続off-curve点、暗黙中点、
  始点がoff-curveの輪郭も省略せず閉じる。
- TrueType複合グリフは子の変換・XYオフセット・実点による位置合わせを
  展開する。循環/過剰深度、範囲外参照、破損したテーブルは例外で拒否する。
  hinting用phantom点による位置合わせは未対応で明示エラー。
- `GlyphVectorRasterizer::render(outline, width, height, scale, origin_x,
  baseline_y)` は公開ベクタデータを指定解像度のalpha画像へ変換する。
  `x=path_x*scale+origin_x`, `y=baseline_y-path_y*scale`。
  MOVE/LINE/QUAD/CUBIC/CLOSEを受け付け、パス効果の結果も入力可能。
- 画像・atlasは同じ輪郭抽出とベクタ描画を利用する。画像のbboxとbearingを
  対にし、負のbearingや右側の張り出しをadvance幅で切り捨てない。
- 曲線は出力画素空間で適応分割。nonzero windingで穴と重なりを保持し、
  横方向は連続座標の区間被覆、縦方向は8サンプルでAAを計算する。
  ラスタ化は最終出力時のみ。ベクタ原本を低解像度bitmapへ置換しない。
- 出力上限4096x4096、輪郭点/分割数/再帰深度に上限を設ける。
  不正な寸法・非有限座標・未対応の輪郭形式は例外で拒否し、偽の字形を返さない。

```cpp
auto outline = TextSvgRenderer(fonts).extract_glyph_outline(font, U'語');
auto alpha = GlyphVectorRasterizer().render(outline, 128, 128,
                                           96.0f / outline.em_size, 8, 104);
// outlineは同じまま、scaleと出力寸法を変えて再描画できる。
```

対象はTrueType `glyf`（可変フォントは既定インスタンス）。CFF/CFF2、
可変軸選択、TrueType命令hinting、複雑な文字組み/shapingは未対応。
高品質な最小文字サイズや実アプリでの読みやすさはビルドだけでは保証しない。
仕様根拠: [OpenType glyf](https://learn.microsoft.com/en-us/typography/opentype/spec/glyf)。
