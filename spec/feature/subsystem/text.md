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

実装は用途別に独立した 3 経路を持つ (共通の `FontLoader` を土台にする)。

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
