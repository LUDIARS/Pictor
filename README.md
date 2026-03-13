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

## ディレクトリ構成

```
Pictor/
├── include/pictor/
│   ├── pictor.h                  # パブリックAPI
│   ├── core/                     # PictorRenderer, 型定義
│   ├── scene/                    # SceneRegistry, ObjectPool, SoAStream
│   ├── memory/                   # アロケータ群
│   ├── update/                   # UpdateScheduler, JobDispatcher
│   ├── batch/                    # BatchBuilder, RadixSort
│   ├── culling/                  # CullingSystem, FlatBVH
│   ├── gpu/                      # GPUDrivenPipeline, GPUBufferManager
│   ├── pipeline/                 # PipelineProfile, RenderPassScheduler
│   └── profiler/                 # Profiler, Overlay
├── src/                          # 実装
├── shaders/                      # Compute Shader (.comp)
├── demo/                         # Vulkan ウィンドウデモ
├── benchmark/                    # 1M Spheres ベンチマーク
└── plan.md                       # 技術設計書
```

## ライセンス

See [LICENSE](LICENSE) for details.
