# Pictor Demo Guide

Pictor が提供する 3 つのデモアプリケーションの解説資料です。
各デモはそれぞれ独立したビルドターゲットとして構成されており、
レンダリングパイプラインの異なる側面を実演します。

---

## 1. Vulkan Window Demo (`pictor_demo`)

### 概要

最もシンプルなデモ。GLFW ウィンドウを開き、Vulkan スワップチェーンを初期化して
クリアカラーアニメーション（色相サイクル）を描画します。
パイプラインが正常に動作していることを視覚的に確認するためのデモです。

### 主な機能

| 機能 | 詳細 |
|------|------|
| ウィンドウ | GLFW 1280x720, VSync ON |
| レンダリング | クリアカラー HSV アニメーション (10秒で1周) |
| Pictor 連携 | 1000 個のテストオブジェクト登録、プロファイラ統計出力 |
| バリデーション | Vulkan Validation Layer 有効 |

### アーキテクチャ

```
GlfwSurfaceProvider → VulkanContext → PictorRenderer
        │                  │                │
     GLFW Window      Swapchain       Data Pipeline
                     Clear Color      (profiler stats)
```

### ビルド & 実行

```bash
cmake -DPICTOR_BUILD_DEMO=ON ..
make pictor_demo
./pictor_demo
```

### ソースファイル

- `demo/main.cpp` — エントリポイント (約210行)

---

## 2. 1M Spheres Benchmark (`pictor_benchmark`)

### 概要

100万個の球体を3D リサージュ曲線上で動かすストレステスト。
GPU Compute Shader による更新 (Level 3) と CPU 並列更新 (Level 1+2) の
両モードをサポートし、データパイプラインの性能を計測します。

### 主な機能

| 機能 | 詳細 |
|------|------|
| オブジェクト数 | 1,000,000 (100x100x100 グリッド) |
| 更新モード | GPU Compute (デフォルト) / CPU (`--cpu` フラグ) |
| レンダリング | SimpleRenderer (icosphere インスタンス描画) |
| プロファイラ | 600フレーム分の統計を JSON/CSV/Chrome Tracing で出力 |
| ヘッドレス | `--headless` でウィンドウなし実行 |

### パフォーマンス指標

- CPU フレーム時間目標: < 0.5ms
- カリング: Frustum + BVH
- バッチング: Radix Sort + Batch Builder
- GPU 駆動パイプライン: Compute Update + Hi-Z Culling

### アーキテクチャ

```
BenchmarkUpdateCallback (IUpdateCallback)
        │
        ▼
PictorRenderer → UpdateScheduler → CullingSystem → BatchBuilder
        │
        ▼
SimpleRenderer (Vulkan instanced draw)
        │
        ▼
Profiler → DataExporter (JSON/CSV/Chrome Tracing)
```

### ビルド & 実行

```bash
cmake -DPICTOR_BUILD_BENCHMARK=ON ..
make pictor_benchmark

# GPU Compute モード (デフォルト)
./pictor_benchmark

# CPU モード
./pictor_benchmark --cpu

# ヘッドレス (データパイプラインのみ)
./pictor_benchmark --headless
```

### ソースファイル

- `benchmark/main.cpp` — エントリポイント (約380行)

---

## 3. PBR Graphics Demo (`pictor_graphics_demo`)

### 概要

PBR (Physically Based Rendering)、シャドウマッピング、グローバルイルミネーション (GI) の
統合動作をデモンストレーションするシーン。Cook-Torrance BRDF による
メタリックマテリアル、PCSS ソフトシャドウ、SSAO、ライトマップベイクを実演します。

### シーン構成

| オブジェクト | メッシュ | マテリアル | 配置 | 種別 |
|-------------|---------|-----------|------|------|
| メタリックキューブ | Cube | Chrome (metalness=0.95, roughness=0.15) | 中央, Y=1.5 | Static, 回転アニメ |
| 地面 | Plane 30x30 | Concrete (metalness=0.0, roughness=0.8) | Y=0 | Static |
| 銅球 | Sphere | Copper (metalness=0.9, roughness=0.25) | 右前方 | Static |
| 金球 | Sphere | Gold (metalness=0.95, roughness=0.35) | 左前方 | Static |
| チタン球 | Sphere | Titanium (metalness=0.85, roughness=0.2) | 後方 | Static |
| ダイナミック球 | Sphere | Teal Emissive (metalness=0.7) | 円運動 | Dynamic |

### ライティング

| ライト | タイプ | 色 | 強度 | 特徴 |
|--------|--------|-----|------|------|
| 太陽光 | Directional | 暖白 (1.0, 0.95, 0.85) | 2.5 | PCSS シャドウ (3カスケード) |
| スポットライト | Spot | 暖色 (1.0, 0.85, 0.6) | 4.0 | コーン20/35度, 周回運動 |
| アンビエント | Hemisphere | 青寄り (0.15, 0.17, 0.25) | 1.0 | 天空+地面バウンス |

### GI システム

- **シャドウ**: CSM 3カスケード, 2048解像度, PCSS フィルタ
- **SSAO**: 32サンプル, radius=0.5, bilateral blur
- **ライトマップベイク**: 静的オブジェクトに対して事前計算
  - `bake_static_gi()` で Shadow + AO + Lightmap を一括ベイク
  - `apply_bake()` で GPU にアップロード

### PBR シェーダ

#### Cook-Torrance BRDF

```
Lo = (kD * albedo / PI + D*G*F / (4*NdotV*NdotL)) * radiance * NdotL
```

- **D** (NDF): GGX 正規分布関数
- **G** (Geometry): Smith's Schlick-GGX
- **F** (Fresnel): Schlick 近似 (`F0 = mix(0.04, albedo, metallic)`)
- **kD**: エネルギー保存 `(1 - F) * (1 - metallic)`

#### スポットライト減衰

```
attenuation = angleFalloff * distanceAttenuation^2
angleFalloff = clamp((cos(angle) - cos(outer)) / (cos(inner) - cos(outer)))
```

#### トーンマッピング

ACES Filmic トーンマッピング + sRGB ガンマ補正

### アーキテクチャ

```
BaseMaterialBuilder (6 PBR materials)
        │
        ▼
PictorRenderer
  ├── SceneRegistry (Static + Dynamic pools)
  ├── GILightingSystem (Shadow + SSAO)
  ├── GIBakeSystem (Static lightmap bake)
  └── CullingSystem (Frustum + BVH)
        │
        ▼
PBRDemoRenderer (Vulkan)
  ├── Cube mesh (24 vertices, 36 indices)
  ├── Floor mesh (4 vertices, 6 indices)
  ├── Sphere mesh (UV sphere, 32x24 subdivisions)
  ├── SceneUBO (lights, camera, shadow VP)
  └── InstanceData SSBO (per-object model + PBR params)
        │
        ▼
pbr_demo.vert → pbr_demo.frag → Framebuffer
```

### ビルド & 実行

```bash
cmake -DPICTOR_BUILD_GRAPHICS_DEMO=ON ..
make pictor_graphics_demo
./pictor_graphics_demo
```

### ソースファイル

- `graphics_demo/main.cpp` — デモアプリケーション + PBRDemoRenderer
- `shaders/pbr_demo.vert` — PBR 頂点シェーダ
- `shaders/pbr_demo.frag` — PBR フラグメントシェーダ (Cook-Torrance BRDF)

---

## ビルドオプション一覧

| CMake オプション | デフォルト | 説明 |
|-----------------|-----------|------|
| `PICTOR_BUILD_DEMO` | ON | Vulkan Window Demo |
| `PICTOR_BUILD_BENCHMARK` | ON | 1M Spheres Benchmark |
| `PICTOR_BUILD_GRAPHICS_DEMO` | ON | PBR Graphics Demo |
| `PICTOR_ENABLE_PROFILER` | ON | プロファイラ有効化 |
| `PICTOR_USE_LARGE_PAGES` | OFF | ラージページサポート |

### 要件

- CMake 3.20+
- C++20 対応コンパイラ
- Vulkan SDK (デモ・ベンチマークに必要)
- GLFW3 (自動ダウンロード対応)
