# Pictor

**Data-Driven Rendering Pipeline Module v2.1**

Pictorは、描画オブジェクトの一元管理と最適な描画パイプラインの自動選択を行うレンダリングパイプラインモジュールです。特定のインタフェースを通じてオブジェクト記述子(ObjectDescriptor)を受け付け、データ駆動設計(DOD)に基づく高効率なレンダリングを実行します。

---

## セットアップ

### 必要要件

| 項目 | 要件 |
|------|------|
| CMake | 3.20 以上 |
| C++ | C++20 対応コンパイラ (GCC / Clang / MSVC) |
| CPU | AVX2 対応 |
| Vulkan SDK | 推奨（GPU機能の有効化に必要） |
| GLFW3 | 自動取得（システム未検出時に FetchContent で取得） |

### 依存関係

| ライブラリ | 取得方法 |
|-----------|---------|
| **Vulkan SDK** | システムインストールが必要（[LunarG](https://vulkan.lunarg.com/) からダウンロード） |
| **GLFW 3.4** | システムにあればそれを使用、なければ CMake の FetchContent で自動ダウンロード |

### ビルド

```bash
git clone <repository-url>
cd Pictor
cmake -B build
cmake --build build
```

#### デモ付きビルド（Vulkan + GLFW）

Vulkan SDK がインストール済みであれば、デモとベンチマークが自動的にビルドされます。
GLFW はシステムになくても FetchContent で自動取得されます。

```bash
# デモ + ベンチマーク付きフルビルド
cmake -B build \
  -DPICTOR_BUILD_DEMO=ON \
  -DPICTOR_BUILD_BENCHMARK=ON \
  -DPICTOR_ENABLE_PROFILER=ON
cmake --build build

# デモ実行
./build/pictor_demo

# ベンチマーク実行（GPU Compute モード）
./build/pictor_benchmark

# ベンチマーク実行（CPU モード）
./build/pictor_benchmark --cpu
```

#### ライブラリのみビルド（デモなし）

```bash
cmake -B build \
  -DPICTOR_BUILD_DEMO=OFF \
  -DPICTOR_BUILD_BENCHMARK=OFF
cmake --build build
```

**ビルドオプション:**

| オプション | デフォルト | 説明 |
|-----------|-----------|------|
| `PICTOR_BUILD_DEMO` | `ON` | Vulkan ウィンドウデモをビルド（Vulkan + GLFW が必要） |
| `PICTOR_BUILD_BENCHMARK` | `ON` | 1M Spheres ベンチマーク実行ファイルをビルド |
| `PICTOR_ENABLE_PROFILER` | `ON` | 組込みプロファイラを有効化 |
| `PICTOR_USE_LARGE_PAGES` | `OFF` | 2MB ラージページによるメモリ確保を有効化 |

**出力:**

- `libpictor.a` — 静的ライブラリ
- `pictor_demo` — Vulkan ウィンドウデモ（オプション、Vulkan + GLFW 必要）
- `pictor_benchmark` — ベンチマーク実行ファイル（オプション）

### プロジェクトへの組み込み

```cmake
add_subdirectory(Pictor)
target_link_libraries(your_app PRIVATE pictor)
```

ヘッダのインクルード:

```cpp
#include <pictor/pictor.h>
```

---

## インプットフォーマット

Pictorはすべての描画対象を **ObjectDescriptor** として受け付けます。

### ObjectDescriptor

```cpp
struct ObjectDescriptor {
    MeshHandle     mesh;       // メッシュID
    MaterialHandle material;   // マテリアルID
    float4x4       transform;  // ワールド変換行列 (64B, キャッシュライン整列)
    AABB           bounds;     // バウンディングボックス (min + max, 24B)
    uint16_t       flags;      // 分類・属性フラグ (下記参照)
    uint64_t       shaderKey;  // レンダーステート識別子
    uint32_t       materialKey;// マテリアル・テクスチャ識別子
    uint8_t        lodLevel;   // LOD レベル (0-255)
};
```

### 属性フラグ

| Bit | 名称 | 説明 |
|-----|------|------|
| 0 | `STATIC` | 静的オブジェクト（トランスフォーム不変） |
| 1 | `DYNAMIC` | 動的オブジェクト（毎フレーム更新） |
| 2 | `GPU_DRIVEN` | GPU Compute Shader で更新・カリング |
| 3 | `CAST_SHADOW` | シャドウマップ書込対象 |
| 4 | `RECEIVE_SHADOW` | シャドウ受け対象 |
| 5 | `TRANSPARENT` | 半透明描画 |
| 6 | `TWO_SIDED` | 両面描画 |
| 7 | `INSTANCED` | インスタンシング描画対象 |
| 8-9 | `LAYER` | レンダリングレイヤー (0-3) |

### 登録 API

```cpp
ObjectId register_object(const ObjectDescriptor& desc);
void     unregister_object(ObjectId id);
void     update_transform(ObjectId id, const float4x4& transform);
```

### Camera

```cpp
struct Camera {
    float4x4 view;
    float4x4 projection;
    float3   position;
    Frustum  frustum;   // 6平面
};
```

---

## パイプライン設計

Pictorのパイプラインは3層構造で構成されます。レイヤー間のデータ受け渡しはSoAストリームの参照（ポインタ + count）で行い、コピーを最小化します。

```
┌─────────────────────────────────────────────────────────┐
│  Front-End Layer (オブジェクト管理)                      │
│  ObjectClassifier → 分類 → Pool配置 (Static/Dynamic/GPU)│
└───────────────────────┬─────────────────────────────────┘
                        │ SoA ストリーム参照
                        ▼
┌─────────────────────────────────────────────────────────┐
│  Middle Layer (バッチング・ソーティング)                  │
│  shaderKey読取 → Radix Sort O(n) → RenderBatch生成      │
│  ※ 実データ移動なし。key-indexペアのみソート             │
└───────────────────────┬─────────────────────────────────┘
                        │ RenderBatch
                        ▼
┌─────────────────────────────────────────────────────────┐
│  Back-End Layer (コマンド発行)                           │
│  PipelineProfile → RenderPass実行順決定 → DrawCommand発行│
└─────────────────────────────────────────────────────────┘
```

### フレーム実行フロー

1. **BeginFrame** — GPU Fence待機、Frame Allocator リセット
2. **UpdateScheduler.update()** — プール種別に応じた更新戦略を選択
   - Static Pool: 更新なし
   - Dynamic Pool (≤10K): CPU 並列ジョブ
   - Dynamic Pool (>10K): CPU 並列 + Non-Temporal Store
   - GPU Driven Pool: Compute Shader 更新
3. **CullingSystem.cull()** — 多段カリング
   - Level 1: CPU Frustum Culling (Flat BVH)
   - Level 2: CPU Occlusion (オプション)
   - Level 3: GPU Hi-Z Culling (Compute Shader)
4. **BatchBuilder.build()** — sortKey生成 → Radix Sort → RenderBatch構築
5. **RenderPassScheduler.execute()** — プロファイルに基づくパス実行とDrawCommand発行
6. **EndFrame** — GPU submit + present、プロファイラ統計集約

---

## レンダリングエンジンの責務

### PictorRenderer

`PictorRenderer` はパイプライン全体を統括するエントリポイントです。

```cpp
RendererConfig config;
config.initial_profile = "Standard";
config.profiler_enabled = true;

PictorRenderer renderer;
renderer.initialize(config);

// オブジェクト登録
ObjectId id = renderer.register_object(desc);

// 毎フレーム
renderer.begin_frame(delta_time);
renderer.render(camera);
renderer.end_frame();
```

### サブシステム一覧

| サブシステム | 責務 |
|-------------|------|
| **MemorySubsystem** | Frame Allocator (バンプ), Pool Allocator, GPU Memory Allocator |
| **SceneRegistry** | Object Pool管理、SoAストリーム確保・拡張 |
| **ObjectClassifier** | 登録時のStatic/Dynamic/GPU Driven判定とPool振り分け |
| **UpdateScheduler** | データ更新戦略選択 (CPU並列 / Compute Update) |
| **BatchBuilder** | shaderKeyストリームからRenderBatch生成 (間接インデックス参照) |
| **CullingSystem** | Flat BVH走査、Frustum Culling、Hi-Z GPU Culling |
| **GPUBufferManager** | メッシュプール、SoA SSBO、インスタンスバッファのサブアロケーション |
| **GPUDrivenPipeline** | Compute Update → Cull → LOD Select → Compact Draw |
| **RenderPassScheduler** | PipelineProfileに基づくパス順序決定と実行 |
| **CommandEncoder** | DrawCommand生成とVkCommandBuffer記録 |
| **Profiler** | FPS, パス別GPU/CPU時間, メモリ統計, オーバーレイUI |
| **DataHandler** | テクスチャ・頂点データの統合管理ファサード |
| **DataQueryAPI** | 外部ツール向け読み取り専用クエリAPI |
| **TextureRegistry** | テクスチャ登録・GPUメモリ管理 |
| **VertexDataUploader** | 柔軟な頂点レイアウトによるメッシュ登録・GPU転送 |
| **BaseMaterialBuilder** | Fluent APIによるPBRマテリアル構築・パス別バリアント生成 |
| **MaterialRegistry** | BuiltMaterial の一元管理・O(1)ルックアップ |
| **GILightingSystem** | シャドウマップ(CSM)・SSAO・Light Probe による GI プリパス |
| **GIBakeSystem** | 静的オブジェクトのオフライン GI ベイク (Shadow/AO/Irradiance/Lightmap) |
| **VulkanContext** | Vulkan インスタンス・デバイス・スワップチェーン管理 |

### SoAデータモデル

全オブジェクトデータは構造体ではなく、同一インデックスで紐付けられた並列配列(SoAストリーム)として管理されます。

| グループ | ストリーム | 用途 |
|---------|-----------|------|
| **A (Hot - カリング)** | `bounds[]`, `visibilityFlags[]` | 毎フレーム読取。Frustum Culling, BVH走査 |
| **B (Hot - ソート)** | `shaderKeys[]`, `sortKeys[]`, `materialKeys[]` | 毎フレーム読取。バッチキー生成、Radix Sort |
| **C (Hot - Transform)** | `transforms[]`, `prevTransforms[]` | Dynamic Poolのみ毎フレーム書込 |
| **D (Cold - メタデータ)** | `meshHandles[]`, `materialHandles[]`, `lodLevels[]`, `flags[]` | 登録・変更時のみアクセス |

### バッチ戦略

| Pool | 描画方式 |
|------|---------|
| Static | Multi Draw Indirect (Base Vertex Offset) |
| Dynamic | Instanced Draw (SSBO Transform Upload) |
| GPU Driven | Indirect Draw Count (GPU がコマンド生成) |

### レンダリングパス

PipelineProfileにより以下のパスを切り替え可能です:

- **Forward** — シンプルなパーオブジェクトシェーディング
- **Forward+** — ライトカリング付きフォワード
- **Deferred** — GBuffer + ライティングパス
- **Hybrid** — Deferred + Forward (半透明)

---

## データハンドラ

`DataHandler` はテクスチャと頂点データの登録・アップロードを統合管理するファサードです。レンダラ内部と外部ツールの双方から利用できます。

### テクスチャ管理

```cpp
// テクスチャ登録（即時アップロードまたは遅延アップロード）
TextureDescriptor tex_desc;
tex_desc.name   = "brick_albedo";
tex_desc.width  = 1024;
tex_desc.height = 1024;
tex_desc.format = TextureFormat::RGBA8_SRGB;

TextureHandle tex = renderer.register_texture(tex_desc);

// 後からデータをアップロード（ミップレベル指定可）
renderer.data_handler().textures().upload_texture_data(tex, pixels, size, /*mip=*/0);
```

### メッシュ管理

```cpp
// 柔軟な頂点レイアウトでメッシュを登録
MeshDataDescriptor mesh_desc;
mesh_desc.name = "cube";
mesh_desc.layout.attributes = {
    {VertexSemantic::POSITION, VertexAttributeType::FLOAT3, 0},
    {VertexSemantic::NORMAL,   VertexAttributeType::FLOAT3, 12},
    {VertexSemantic::TEXCOORD0,VertexAttributeType::FLOAT2, 24},
};
mesh_desc.vertex_data      = vertices;
mesh_desc.vertex_data_size = sizeof(vertices);
mesh_desc.vertex_count     = 24;

MeshHandle mesh = renderer.register_mesh_data(mesh_desc);
```

### 外部クエリAPI

レベルエディタやアセットブラウザ等の外部ツール向けに、読み取り専用のクエリAPIを提供します。

```cpp
DataQueryAPI query = renderer.create_query_api();

// サマリー取得
DataSummary summary = query.get_summary();

// テクスチャ一覧をイテレーション
query.for_each_texture([](const TextureInfo& info) {
    printf("Texture: %s (%dx%d)\n", info.name.c_str(), info.width, info.height);
});

// JSON エクスポート（ツール連携用）
std::string json = query.export_json();
```

---

## マテリアルシステム

`BaseMaterialBuilder` は Fluent API でPBRマテリアルを構築し、レンダーパスごとに最適化されたバリアントを自動生成します。

### マテリアル構築

```cpp
auto mat = BaseMaterialBuilder()
    .albedo(tex_albedo)
    .normal_map(tex_normal)
    .metallic_value(0.0f)
    .roughness_value(0.8f)
    .build(registry.allocate_handle());
```

### パス別バリアント

各マテリアルは登録時にパスごとのバリアントを自動生成します。不要なテクスチャバインディングやフィーチャーフラグが自動的にストリップされ、シェーダーパーミュテーションが最小化されます。

| パスタイプ | 使用されるフィーチャー |
|-----------|---------------------|
| Shadow / Depth-Only | ALPHA_TEST, TWO_SIDED のみ |
| GI | ALBEDO_MAP, EMISSIVE_MAP, ALPHA_TEST, TWO_SIDED, VERTEX_COLOR |
| Opaque / Transparent | 全フィーチャー |

---

## グローバルイルミネーション

### GILightingSystem

マテリアルシェーダから独立したGIプリパスを実行し、結果を読み取り専用テクスチャ/SSBOとして後続パスに提供します。

```
ShadowMapGen → SSAO → GI Probes → (既存シェーダが結果を読み取り)
```

```cpp
// ディレクショナルライト設定
DirectionalLight sun;
sun.direction = {0.5f, -1.0f, 0.3f};
sun.intensity = 1.0f;
renderer.set_directional_light(sun);

// GI設定
GIConfig gi_config;
gi_config.shadow.cascade_count = 3;
gi_config.shadow.resolution    = 2048;
gi_config.ssao.sample_count    = 32;
gi_config.ssao_enabled         = true;
gi_config.gi_probes_enabled    = false; // オプトイン
renderer.set_gi_config(gi_config);
```

### GIBakeSystem

静的オブジェクトに対して高品質なオフラインGIベイクを実行し、ランタイムのGIパスをスキップできます。

```cpp
// ブロッキングベイク
GIBakeResult result = renderer.bake_static_gi();

// プログレス付きベイク
GIBakeResult result = renderer.bake_static_gi([](float progress, const char* stage) {
    printf("[%.0f%%] %s\n", progress * 100, stage);
    return true; // false でキャンセル
});

// 結果をGPUに適用
renderer.apply_bake(result);

// ファイルへの保存/読み込み
renderer.save_bake("scene_gi.bin", result);
GIBakeResult loaded = renderer.load_bake("scene_gi.bin");
```

**ベイクターゲット:**

| ターゲット | 説明 |
|-----------|------|
| SHADOW_MAP | 静的シャドウ深度 + カスケードフラグ |
| AMBIENT_OCCLUSION | オブジェクト空間AO（SSAOより高品質） |
| PROBE_IRRADIANCE | Light Probeグリッドからの放射照度(SH L2) |
| LIGHTMAP | 直接光 + 間接光の統合ライトマップ |

---

## サーフェスプロバイダ

`ISurfaceProvider` インタフェースにより、Pictorをウィンドウシステムから分離しています。

### 組み込みモード（外部ウィンドウ）

ホストアプリケーションが `ISurfaceProvider` を実装し、Pictorに渡します。

```cpp
class MyWindowProvider : public pictor::ISurfaceProvider {
public:
    NativeWindowHandle get_native_handle() const override { /* ... */ }
    SwapchainConfig get_swapchain_config() const override { /* ... */ }
};
```

### スタンドアロンモード（GLFW）

デモ・ツール向けにGLFWベースの実装 `GlfwSurfaceProvider` を同梱しています。

```cpp
GlfwSurfaceProvider surface;
surface.create({.width = 1280, .height = 720, .title = "Pictor Demo"});
```

**対応プラットフォーム:** Win32, Xlib, XCB, Wayland, macOS (Metal), Android

---

## ディレクトリ構成

```
Pictor/
├── include/pictor/
│   ├── pictor.h                  # パブリックAPI (全ヘッダ集約)
│   ├── core/                     # PictorRenderer, 型定義
│   ├── scene/                    # SceneRegistry, ObjectPool, SoAStream
│   ├── memory/                   # アロケータ群
│   ├── update/                   # UpdateScheduler, JobDispatcher
│   ├── batch/                    # BatchBuilder, RadixSort
│   ├── culling/                  # CullingSystem, FlatBVH
│   ├── gpu/                      # GPUDrivenPipeline, GPUBufferManager
│   ├── pipeline/                 # PipelineProfile, RenderPassScheduler, CommandEncoder
│   ├── profiler/                 # Profiler, Overlay, DataExporter
│   ├── data/                     # DataHandler, TextureRegistry, VertexDataUploader, DataQueryAPI
│   ├── material/                 # BaseMaterialBuilder, MaterialProperty, MaterialRegistry
│   ├── gi/                       # GILightingSystem, GIBakeSystem
│   └── surface/                  # ISurfaceProvider, VulkanContext, GlfwSurfaceProvider
├── src/                          # 実装
├── shaders/                      # Compute Shader (.comp), サンプルシェーダ
├── fonts/                        # デフォルトフォント (default.ttf)
├── demo/                         # Vulkan ウィンドウデモ
├── benchmark/                    # 1M Spheres ベンチマーク
├── docs/
│   ├── api/                      # 外部インタフェース・クラス定義ドキュメント
│   └── design/                   # 設計ドキュメント (WebGL バックエンド等)
└── plan.md                       # 技術設計書
```

## フォント

デモで使用するデフォルトフォントは `fonts/default.ttf` に配置されています。ビルド時に自動的にビルドディレクトリへコピーされます。

- **使用フォント:** [WDXL Lubrifont JP N](https://fonts.google.com/specimen/WDXL+Lubrifont+TC+N) (Google Fonts)
- **ライセンス:** SIL Open Font License 1.1

カスタムフォントを使用する場合は、`fonts/default.ttf` または `fonts/default.otf` を置き換えてください。

---

## ライセンス

See [LICENSE](LICENSE) for details.
