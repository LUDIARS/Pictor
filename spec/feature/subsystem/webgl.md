# WebGL — WebGL2 (Emscripten) バックエンド

> 実装: `include/pictor/webgl/` (4), `src/webgl/`、デモ `demo/webgl/`

Vulkan と**並行**の独立レンダリングバックエンド (WebGL2 / GLSL ES 3.00 via Emscripten)。
現状は共通抽象 (`RenderBackend` trait) を持たない最小リファレンス実装。

## 構成

| クラス / struct | 役割 |
|---|---|
| `WebGLContext` | HTML canvas に紐づく WebGL2 context のライフサイクル (Emscripten handle 所有) |
| `WebGLBufferManager` | VBO/IBO/UBO/VAO 管理 |
| `WebGLShaderManager` | GLSL ES 3.00 program のコンパイル/リンク/キャッシュ + uniform 引き |
| `WebGLRenderer` | 最小インスタンスドレンダラ (icosphere + per-instance transform/color + CPU frustum cull) |

設定: `WebGLContextConfig` / `WebGLCapabilities` (max texture/extension) / `WebGLRendererConfig` / `WebGLInstanceData` (80B = 4x4 + RGBA) / `WebGLFrameStats`。

## 主要 API

```cpp
// WebGLContext
bool initialize(const WebGLContextConfig&);
void begin_frame(float r,g,b,a); void end_frame();
void resize(uint32_t w, uint32_t h);
// WebGLRenderer
bool initialize(const WebGLRendererConfig&);
ObjectId register_object(const ObjectDescriptor&);
void update_transform(ObjectId, const float4x4&);
void render(const float* view, const float* projection);
void set_camera(const float3& eye, const float3& target, const float3& up);
const WebGLFrameStats& stats() const;
```

## Vulkan 経路との関係

- **並行・非抽象**: Vulkan 側 `SimpleRenderer` と機能対応するが共通 IF はまだ無い。ヘッダコメントに「将来 `RenderBackend` trait で `PictorRenderer` に統合予定」と明記
- **サポート**: instanced (transform+color) / icosphere 生成 / CPU frustum cull / depth・cull・blend / UBO / half-Lambert + rim の簡易ライティング / HiDPI / extension 検出
- **未サポート (vs Vulkan)**: deferred / compute / post-process / テクスチャ sampling / push constant / dynamic pipeline。**意図的に subset** のリファレンス実装

## ビルド / プラットフォーム gating

```cmake
option(PICTOR_BUILD_WEBGL "Build WebGL2 backend (Emscripten)" OFF)  # 既定 OFF
# ON で src/webgl/*.cpp をコンパイル、PICTOR_HAS_WEBGL=1、-sUSE_WEBGL2=1 -sFULL_ES3=1
```

- C++ ガード: `#ifdef PICTOR_HAS_WEBGL`
- **Emscripten 専用** (`emcxx`/`emar`、`-sUSE_WEBGL2=1` 前提)

## 依存 / デモ

`<GLES3/gl3.h>`、`<emscripten/html5.h>`、`<emscripten.h>` (Vulkan ヘッダなし)。デモ `demo/webgl/{main.cpp,shell.html}` = 10×10 アニメ icosphere + 軌道カメラ + stats overlay。shader は renderer に埋込。専用 level editor は無し。関連: [surface.md](surface.md) (Vulkan 経路)。
