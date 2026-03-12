# Pictor WebGL バックエンド設計書

## 1. 概要

Pictor の描画バックエンドとして WebGL (WebGL2 / WebGPU 拡張対応) を追加し、
ブラウザ上での描画を実現する。Rust + WebAssembly (wasm32-unknown-unknown) で実装し、
既存の C++ コアとはデータフォーマット互換を維持する。

### 1.1 目的

| 目的 | 詳細 |
|------|------|
| ブラウザ描画 | デスクトップアプリなしで Pictor シーンを可視化 |
| クロスプラットフォーム | OS に依存しない描画パス |
| 組み込み用途 | Web アプリケーションへの埋め込みレンダラ |
| 将来の WebGPU 移行パス | WebGL2 → WebGPU への段階的移行を見据えた設計 |

### 1.2 スコープ

- **対象**: WebGL2 (ES 3.0 相当)
- **言語**: Rust (stable)
- **ターゲット**: `wasm32-unknown-unknown`
- **JS 連携**: `wasm-bindgen` + `web-sys`
- **対象外**: WebGPU 実装（将来ブランチで対応）

---

## 2. アーキテクチャ

```
┌─────────────────────────────────────────────────────────────────┐
│  Host Web Application (JavaScript / TypeScript)                 │
│  ┌───────────────┐  ┌──────────────────────────────────┐       │
│  │  <canvas>      │  │  pictor_webgl.js (wasm-bindgen)  │       │
│  │  element       │  │  - init(canvas)                  │       │
│  │                │  │  - load_scene(data)              │       │
│  └───────┬───────┘  │  - render_frame(dt)               │       │
│          │          │  - resize(w, h)                   │       │
│          │          └──────────┬───────────────────────┘       │
│          │                     │  wasm-bindgen FFI             │
└──────────┼─────────────────────┼───────────────────────────────┘
           │                     │
┌──────────┼─────────────────────┼───────────────────────────────┐
│  WASM Module (Rust)            │                                │
│          │                     │                                │
│  ┌───────▼─────────────────────▼───────────────────────┐       │
│  │  pictor-webgl crate                                  │       │
│  │                                                      │       │
│  │  ┌──────────────┐  ┌─────────────┐  ┌────────────┐ │       │
│  │  │ WebGlContext  │  │ ShaderMgr   │  │ BufferMgr  │ │       │
│  │  │ (web-sys)     │  │ (GLSL ES)   │  │ (VBO/IBO)  │ │       │
│  │  └──────┬───────┘  └──────┬──────┘  └─────┬──────┘ │       │
│  │         │                 │                │        │       │
│  │  ┌──────▼─────────────────▼────────────────▼──────┐ │       │
│  │  │ RenderPipeline                                 │ │       │
│  │  │ - Batch → DrawCall 変換                        │ │       │
│  │  │ - Instanced Drawing (ANGLE_instanced_arrays)   │ │       │
│  │  │ - Multi-Draw Emulation                         │ │       │
│  │  └────────────────────────────────────────────────┘ │       │
│  │                                                      │       │
│  │  ┌────────────────────────────────────────────────┐ │       │
│  │  │ pictor-core (Rust port / shared data types)    │ │       │
│  │  │ - SoA Streams, AABB, float4x4                  │ │       │
│  │  │ - Frustum Culling (CPU, WASM SIMD)              │ │       │
│  │  │ - Radix Sort (key-index)                        │ │       │
│  │  │ - Batch Builder                                 │ │       │
│  │  └────────────────────────────────────────────────┘ │       │
│  └──────────────────────────────────────────────────────┘       │
└─────────────────────────────────────────────────────────────────┘
```

---

## 3. クレート構成

```
pictor-web/
├── Cargo.toml                   # workspace
├── crates/
│   ├── pictor-core/             # プラットフォーム非依存のコアロジック
│   │   ├── Cargo.toml
│   │   └── src/
│   │       ├── lib.rs
│   │       ├── types.rs         # float3, float4x4, AABB, ObjectDescriptor
│   │       ├── frustum.rs       # Frustum culling (CPU)
│   │       ├── radix_sort.rs    # Radix Sort (key-index pairs)
│   │       ├── batch.rs         # BatchBuilder
│   │       ├── soa.rs           # SoA stream management
│   │       └── scene.rs         # SceneRegistry
│   │
│   └── pictor-webgl/            # WebGL2 バックエンド
│       ├── Cargo.toml
│       └── src/
│           ├── lib.rs           # wasm-bindgen エントリポイント
│           ├── context.rs       # WebGL2RenderingContext ラッパー
│           ├── shader.rs        # GLSL ES 3.00 シェーダ管理
│           ├── buffer.rs        # VBO / IBO / UBO 管理
│           ├── texture.rs       # テクスチャ管理
│           ├── pipeline.rs      # Draw 発行 + state 管理
│           ├── instancing.rs    # Instanced Draw 実装
│           └── camera.rs        # View/Projection 行列
│
├── www/                          # デモ HTML + JS
│   ├── index.html
│   ├── main.js
│   └── style.css
│
└── shaders/                      # GLSL ES 3.00 シェーダ
    ├── basic.vert
    ├── basic.frag
    ├── instanced.vert
    └── instanced.frag
```

### 3.1 依存関係

```toml
# pictor-core/Cargo.toml
[dependencies]
# no_std 互換、プラットフォーム非依存

# pictor-webgl/Cargo.toml
[dependencies]
pictor-core = { path = "../pictor-core" }
wasm-bindgen = "0.2"
web-sys = { version = "0.3", features = [
    "Window", "Document", "HtmlCanvasElement",
    "WebGl2RenderingContext", "WebGlProgram", "WebGlShader",
    "WebGlBuffer", "WebGlUniformLocation", "WebGlVertexArrayObject",
    "WebGlTexture", "WebGlFramebuffer",
] }
js-sys = "0.3"
glam = { version = "0.29", features = ["bytemuck"] }
bytemuck = { version = "1", features = ["derive"] }
```

---

## 4. 主要コンポーネント詳細

### 4.1 pictor-core (Rust ポート)

C++ の Pictor コアロジックのうち、GPU 非依存な部分を Rust に移植する。
C++ 側とのデータレイアウト互換性を `#[repr(C)]` で保証する。

```rust
// types.rs
#[repr(C)]
#[derive(Clone, Copy, Default, bytemuck::Pod, bytemuck::Zeroable)]
pub struct Float3 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

#[repr(C, align(64))]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct Float4x4 {
    pub m: [[f32; 4]; 4],
}

#[repr(C)]
#[derive(Clone, Copy, Default, bytemuck::Pod, bytemuck::Zeroable)]
pub struct Aabb {
    pub min: Float3,
    pub max: Float3,
}

pub type ObjectId = u32;
pub type MeshHandle = u32;
pub type MaterialHandle = u32;
```

### 4.2 WebGL2 コンテキスト

```rust
// context.rs
use web_sys::WebGl2RenderingContext as GL;

pub struct WebGlContext {
    gl: GL,
    canvas_width: u32,
    canvas_height: u32,
    max_texture_units: i32,
    max_uniform_buffer_bindings: i32,
    supports_instancing: bool, // WebGL2 では常に true
}

impl WebGlContext {
    /// <canvas> 要素から WebGL2 コンテキストを取得
    pub fn from_canvas(canvas: &HtmlCanvasElement) -> Result<Self, String> { ... }

    /// ビューポート設定
    pub fn set_viewport(&self, w: u32, h: u32) { ... }

    /// フレーム開始 (clear)
    pub fn begin_frame(&self, r: f32, g: f32, b: f32) { ... }
}
```

### 4.3 シェーダ管理

WebGL2 (GLSL ES 3.00) 用のシェーダペアを管理する。

```glsl
// shaders/instanced.vert — GLSL ES 3.00
#version 300 es
precision highp float;

// Per-vertex attributes
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;

// Per-instance attributes (mat4 = 4 × vec4)
layout(location = 2) in vec4 a_model_0;
layout(location = 3) in vec4 a_model_1;
layout(location = 4) in vec4 a_model_2;
layout(location = 5) in vec4 a_model_3;

// Per-instance colour / material ID
layout(location = 6) in vec4 a_color;

uniform mat4 u_view_projection;

out vec3 v_normal;
out vec4 v_color;

void main() {
    mat4 model = mat4(a_model_0, a_model_1, a_model_2, a_model_3);
    gl_Position = u_view_projection * model * vec4(a_position, 1.0);
    v_normal = mat3(model) * a_normal;
    v_color  = a_color;
}
```

### 4.4 インスタンス描画

Pictor の Batch → WebGL2 の `drawElementsInstanced` への変換。

```rust
// instancing.rs
pub struct InstanceBatch {
    pub mesh: MeshHandle,
    pub instance_count: u32,
    pub transform_buffer: WebGlBuffer,  // mat4[] packed as vec4×4
    pub color_buffer: WebGlBuffer,      // vec4[]
}

impl InstanceBatch {
    /// RenderBatch から WebGL2 Instanced Draw を発行
    pub fn draw(&self, gl: &GL, vao: &WebGlVertexArrayObject) {
        gl.bindVertexArray(vao);

        // Transform attributes (locations 2-5)
        // 各 vec4 列に divisor=1 を設定
        for i in 0..4 {
            let loc = 2 + i;
            gl.enableVertexAttribArray(loc);
            gl.vertexAttribDivisor(loc, 1);
        }

        // Color attribute (location 6)
        gl.enableVertexAttribArray(6);
        gl.vertexAttribDivisor(6, 1);

        gl.drawElementsInstanced(
            GL::TRIANGLES,
            self.index_count as i32,
            GL::UNSIGNED_INT,
            0,
            self.instance_count as i32,
        );
    }
}
```

### 4.5 Frustum Culling (WASM SIMD)

WebAssembly SIMD (128-bit) を活用した高速カリング。

```rust
// frustum.rs
#[cfg(target_arch = "wasm32")]
use core::arch::wasm32::*;

pub fn cull_aabbs_simd(
    planes: &[Plane; 6],
    bounds: &[Aabb],
    visibility: &mut [u8],
) {
    // WASM SIMD: 4 つの AABB を同時にテスト
    // f32x4 で plane.normal.dot(p-vertex) + d を並列計算
    for chunk in bounds.chunks(4) {
        // ... SIMD 実装 ...
    }
}
```

---

## 5. JavaScript API 設計

```typescript
// TypeScript 型定義 (pictor_webgl.d.ts)

export class PictorWebGL {
    /** canvas 要素を指定して初期化 */
    static init(canvas: HTMLCanvasElement): PictorWebGL;

    /** シーンデータ (バイナリ) をロード */
    loadScene(data: ArrayBuffer): void;

    /** オブジェクトを登録 */
    registerObject(desc: ObjectDescriptor): number;

    /** オブジェクトの Transform を更新 */
    updateTransform(id: number, matrix: Float32Array): void;

    /** 1 フレーム描画 */
    renderFrame(deltaTime: number): void;

    /** canvas リサイズ通知 */
    resize(width: number, height: number): void;

    /** カメラ設定 */
    setCamera(eye: Float32Array, target: Float32Array, up: Float32Array): void;

    /** パフォーマンス統計取得 */
    getStats(): FrameStats;

    /** リソース解放 */
    destroy(): void;
}

interface ObjectDescriptor {
    mesh: number;
    material: number;
    transform: Float32Array;  // 16 floats (4x4 matrix)
    flags: number;
}

interface FrameStats {
    fps: number;
    frameTimeMs: number;
    drawCalls: number;
    triangles: number;
    visibleObjects: number;
    totalObjects: number;
}
```

### 5.1 使用例

```html
<canvas id="pictor-canvas" width="1280" height="720"></canvas>
<script type="module">
import init, { PictorWebGL } from './pictor_webgl.js';

await init();  // WASM モジュールロード

const canvas = document.getElementById('pictor-canvas');
const renderer = PictorWebGL.init(canvas);

// シーン登録
for (let i = 0; i < 1000; i++) {
    const transform = new Float32Array(16);
    // ... identity + translation ...
    renderer.registerObject({
        mesh: 0,
        material: 0,
        transform,
        flags: 0x02,  // DYNAMIC
    });
}

// メインループ
let lastTime = performance.now();
function frame(now) {
    const dt = (now - lastTime) / 1000;
    lastTime = now;

    renderer.renderFrame(dt);

    requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
</script>
```

---

## 6. WebGL2 制約と対策

| C++ Pictor 機能 | WebGL2 制約 | 対策 |
|------------------|-------------|------|
| Compute Shader (GPU Update/Cull) | WebGL2 未サポート | CPU (WASM SIMD) でカリング・ソート実行 |
| SSBO | WebGL2 未サポート | UBO (std140) + テクスチャバッファ で代替 |
| Multi-Draw Indirect | WebGL2 未サポート | ループ + `drawElementsInstanced` で発行 |
| 64-bit Sort Key | JS の Number は 53-bit | Rust 内部で u64、JS 境界は BigInt or 2×u32 |
| Non-Temporal Store | WASM 未対応 | 通常の memcpy（WASM 最適化に任せる） |
| 1M Objects | 描画コスト大 | LOD 強化 + CPU カリングで描画数抑制、目標 10K draw calls/frame |
| Large Page | WASM Memory 非対応 | 標準 WASM Linear Memory |

---

## 7. ビルド・デプロイ

### 7.1 ビルドコマンド

```bash
# 開発ビルド
wasm-pack build crates/pictor-webgl --target web --dev

# リリースビルド (最適化 + wasm-opt)
wasm-pack build crates/pictor-webgl --target web --release

# WASM SIMD 有効化 (Nightly)
RUSTFLAGS="-C target-feature=+simd128" \
  wasm-pack build crates/pictor-webgl --target web --release
```

### 7.2 出力物

```
pkg/
├── pictor_webgl_bg.wasm    # WASM バイナリ (~200KB gzip 目標)
├── pictor_webgl.js          # JS グルーコード
├── pictor_webgl.d.ts        # TypeScript 型定義
└── package.json             # npm パッケージメタデータ
```

### 7.3 WASM サイズ最適化

```toml
# Cargo.toml
[profile.release]
opt-level = "s"         # サイズ最適化
lto = true              # Link-Time Optimization
codegen-units = 1
strip = true

[profile.release.package.pictor-webgl]
opt-level = "z"         # 最大サイズ圧縮
```

---

## 8. パフォーマンス目標

| メトリクス | 目標値 | 備考 |
|-----------|--------|------|
| 初期化時間 | < 500ms | WASM ロード + コンパイル含む |
| 10K オブジェクト描画 | 60fps | Instanced Drawing 使用 |
| 100K オブジェクト描画 | 30fps | LOD + Frustum Culling 必須 |
| WASM バイナリサイズ | < 500KB (gzip) | wasm-opt + strip 適用 |
| メモリ使用量 | < 64MB (10K objects) | SoA レイアウト維持 |
| JS ↔ WASM コピー | 最小化 | SharedArrayBuffer 活用検討 |

---

## 9. 将来の WebGPU 拡張パス

WebGL2 バックエンドの設計は WebGPU への移行を考慮する。

```
pictor-webgl (現在)        pictor-webgpu (将来)
─────────────────          ──────────────────
drawElementsInstanced  →   renderPass.draw (Indirect)
UBO (std140)           →   Storage Buffer
GLSL ES 3.00           →   WGSL
CPU Frustum Cull       →   Compute Shader Cull
CPU Radix Sort         →   Compute Shader Sort
```

### 9.1 共通抽象レイヤー

`pictor-core` クレートはバックエンド非依存に設計し、
WebGL2 / WebGPU / ネイティブ Vulkan すべてから利用可能にする。

```rust
// pictor-core/src/backend.rs (将来)
pub trait RenderBackend {
    type Buffer;
    type Shader;
    type Pipeline;

    fn create_buffer(&self, size: usize, usage: BufferUsage) -> Self::Buffer;
    fn upload_data(&self, buffer: &Self::Buffer, data: &[u8]);
    fn draw_instanced(&self, mesh: &MeshData, instances: u32);
}
```

---

## 10. 実装フェーズ

### Phase 1: 基盤 (MVP)
- [ ] `pictor-core` クレート作成 (types, frustum, batch)
- [ ] `pictor-webgl` クレート作成 (context, shader, basic draw)
- [ ] 静的シーン描画 (単一メッシュ + 単一マテリアル)
- [ ] デモ HTML ページ

### Phase 2: インスタンシング
- [ ] Instanced Drawing 実装
- [ ] Pictor BatchBuilder → WebGL2 Draw 変換
- [ ] 1K〜10K オブジェクト描画検証

### Phase 3: カリング・最適化
- [ ] CPU Frustum Culling (WASM SIMD)
- [ ] LOD 選択
- [ ] パフォーマンスプロファイリング

### Phase 4: 統合
- [ ] npm パッケージ公開準備
- [ ] TypeScript 型定義の充実
- [ ] E2E テスト (headless Chrome + WebGL)
- [ ] ドキュメント整備
