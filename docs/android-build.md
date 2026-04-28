# Pictor Android (NDK) ビルド セットアップ計画

Pictor を **Android NDK でネイティブライブラリとしてビルドする** ために
必要なツールチェーン、コード改修、段階的マイルストーンを整理する。
iOS / Metal / MoltenVK は本書の対象外。

現時点では **何もビルドは成功していない**。本書は作業対象のリストアップと
実装方針の事前合意を目的とする。

---

## 0. TL;DR

| 項目 | 要否 | 状態 |
|------|------|------|
| Android NDK + Vulkan ヘッダ | 必須 | 未導入 |
| Android SDK + build-tools | 実機 APK で必須、lib 単体なら不要 | 未導入 |
| CMake Android toolchain 経由のクロスビルド | 必須 | ハンドリング未実装 |
| `VK_USE_PLATFORM_ANDROID_KHR` 分岐 (CMake) | 必須 | **欠落** — コード側の `#ifdef` はあるが CMake から渡していない |
| GLFW の Android 時の無効化 | 必須 | 未対応 (無条件 FetchContent) |
| `-mavx2 / /arch:AVX2` の ARM64 回避 | 必須 | 未対応 (ARM で死ぬ) |
| `AndroidSurfaceProvider` 実装 | 必須 | 未実装 |
| glslc (コンピュートシェーダ焼き込み) | 必須 | NDK 同梱で OK |
| JNI ブリッジ / Gradle プロジェクト | 実機で必須、静的 lib 単体なら後回し可 | 未実装 |
| Rive Renderer の ARM64 ビルド | `PICTOR_ENABLE_RIVE=OFF` なら不要 | 未対応 |

着手順推奨: **Phase 1 (CMake で arm64-v8a の `libpictor.a` が通る) → Phase 2
(headless demo が emulator で動く) → Phase 3 (Gradle + JNI で APK に載る)**。

---

## 1. 前提ツールチェーン

ホスト: Windows / macOS / Linux のいずれでも可。Windows は WSL2 上 Linux が最も確実。

### 1.1 インストールするもの

| ツール | 推奨バージョン | 備考 |
|--------|----------------|------|
| **Android NDK** | r26d 以上 (2025 年時点) | Vulkan 1.3 ヘッダ / clang 17 / C++20 対応。r25c でも可だが古い |
| **CMake** | 3.22 以上 | `CMAKE_ANDROID_STL_TYPE` / `CMAKE_ANDROID_API` が安定して効く |
| **Ninja** | 1.11+ | Android toolchain のデフォルト生成系 |
| **JDK** | 17 (Temurin) | Gradle / AGP 8.x が要求 |
| **Android SDK (optional)** | cmdline-tools latest | Phase 3 の APK パッケージングで必須、Phase 1-2 は不要 |
| **Android Platform API** | API 33+ | Vulkan 1.3 は API 33 から完全露出。最低 API 26 (Vulkan 1.1) |
| **glslc / spirv-tools** | NDK 同梱 (`prebuilt/<host>/bin/glslc`) | Pictor の compute shader を SPIR-V に焼く |
| **adb (Platform-Tools)** | 任意 | Phase 2 以降で実機デプロイ |

### 1.2 環境変数

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk-r26d
export PATH=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
```

Windows なら `setx ANDROID_NDK_HOME …` + `PATH` 追加、もしくは `tools/env.ps1` のようなラッパを生やす。

### 1.3 ABI 方針

| ABI | 優先度 | 理由 |
|-----|--------|------|
| `arm64-v8a`   | **必須** | 現行 Android 端末の 99%+ 。primary target |
| `armeabi-v7a` | 任意 | 旧端末サポート。サイズと保守コストで外す判断もあり |
| `x86_64`      | 任意 | Android Emulator を x86_64 で回すなら必要 |
| `x86`         | 外す | Emulator 専用、かつ 32bit |

**推奨**: `arm64-v8a` + `x86_64` (エミュレータ向け) の 2 ABI に絞って開始。

---

## 2. 現状コードベースの Android ブロッカー

### 2.1 CMake レベル

`CMakeLists.txt` 内、Android を考慮していない箇所:

| 行 | 問題 | 対処 |
|----|------|------|
| 17-18 `find_package(Vulkan QUIET)` | NDK の Vulkan は `find_package` で拾えない。ヘッダ + libvulkan.so は NDK が同梱するが CMake モジュールがない | `if(ANDROID)` 分岐で NDK パスから直接リンクする |
| 20-37 `FetchContent` で GLFW を**常に**取得 | **GLFW は Android を native target として持たない**。クロスビルドが失敗する | `if(NOT ANDROID)` で全体をガード |
| 156-163 `VK_USE_PLATFORM_*` の分岐 | WIN32 / UNIX NOT APPLE / APPLE の 3 つのみ。**Android 分岐なし** | `if(ANDROID)` 追加、`VK_USE_PLATFORM_ANDROID_KHR=1` を定義 |
| 189 `-mavx2` / 191 `/arch:AVX2` | **ARM64 で clang がエラー**。AVX2 は x86 専用命令 | `if(NOT ANDROID AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch")` のガード。代わりに ARM64 には `-mfpu=neon` を付けるか、SIMD 抽象を用意 |
| 194-196 `target_link_libraries(pictor PUBLIC glfw)` | Android では glfw ターゲットが存在しない | `if(glfw3_FOUND)` のまま Android で false になれば OK (GLFW の取得自体を止める前提) |
| demo/ 群のうち GLFW / Vulkan SDK 前提のもの | Android 上では build できない | `if(ANDROID)` で `pictor_mobile_demo` (headless) と `pictor_material_serializer_demo` のみ有効化 |

### 2.2 ソースコード

| ファイル | 状態 | 対処 |
|----------|------|------|
| `src/surface/vulkan_context.cpp` | `#ifdef VK_USE_PLATFORM_ANDROID_KHR` で `vkCreateAndroidSurfaceKHR` 呼ぶ箇所は **既にある** | CMake で define を渡せば生きる |
| `include/pictor/surface/surface_provider.h` | `NativeWindowHandle::Android` に `ANativeWindow*` フィールドが既にある | 追加実装不要 |
| `AndroidSurfaceProvider` クラス | **未実装** | `include/pictor/surface/android_surface_provider.h` + cpp を新設 |
| `src/memory/*` の AVX2 intrinsic 使用箇所 | 要確認 — `memory_subsystem.cpp` などで `__m256` を使っていれば ARM64 build で失敗 | スカラフォールバック or `#if defined(__x86_64__)` ガードを入れる |
| `third_party/stb` | header-only、問題なし | — |

### 2.3 Rive (`PICTOR_ENABLE_RIVE=ON` のとき)

- Rive Renderer は premake5 build。**Android (arm64) 向け prebuilt は公式に提供されていない**
- 自前で NDK 経由のクロスビルドが必要 → 保守コスト高
- **推奨**: Phase 1 / 2 では `PICTOR_ENABLE_RIVE=OFF` 固定。Phase 3 以降で必要になれば別途

---

## 3. 必要な CMake 改修 (Phase 1)

### 3.1 トップ CMakeLists.txt の `if(ANDROID)` ブロック

```cmake
# === Android cross-build early bail-out ===
if(ANDROID)
    # Vulkan は NDK 同梱 — find_package ではなく直接リンク
    set(PICTOR_VULKAN_LIBRARY vulkan)
    set(PICTOR_VULKAN_INCLUDE_DIR "${ANDROID_NDK}/sources/third_party/vulkan/src/include")

    # GLFW / Vulkan SDK CMake モジュールを使わない
    set(glfw3_FOUND FALSE)
    set(Vulkan_FOUND TRUE CACHE BOOL "" FORCE)
    set(Vulkan_INCLUDE_DIRS "${PICTOR_VULKAN_INCLUDE_DIR}")
    set(Vulkan_LIBRARIES   "${PICTOR_VULKAN_LIBRARY}")

    # GLFW FetchContent を触らない
    set(PICTOR_SKIP_GLFW ON)
endif()

if(NOT ANDROID)
    find_package(Vulkan QUIET)
endif()

if(NOT PICTOR_SKIP_GLFW)
    # 既存の FetchContent ブロック (glfw)
endif()
```

### 3.2 プラットフォーム define の Android 分岐

```cmake
if(Vulkan_FOUND)
    target_link_libraries(pictor PUBLIC Vulkan::Vulkan)
    target_compile_definitions(pictor PUBLIC PICTOR_HAS_VULKAN=1)
    if(ANDROID)
        target_compile_definitions(pictor PUBLIC VK_USE_PLATFORM_ANDROID_KHR=1)
    elseif(WIN32)
        target_compile_definitions(pictor PUBLIC VK_USE_PLATFORM_WIN32_KHR=1 NOMINMAX)
    elseif(UNIX AND NOT APPLE)
        target_compile_definitions(pictor PUBLIC VK_USE_PLATFORM_XLIB_KHR=1)
    elseif(APPLE)
        target_compile_definitions(pictor PUBLIC VK_USE_PLATFORM_MACOS_MVK=1)
    endif()
endif()

# Android では android + log をリンク (NDK 同梱)
if(ANDROID)
    target_link_libraries(pictor PUBLIC android log)
endif()
```

### 3.3 AVX2 フラグの条件化

```cmake
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pictor PRIVATE -Wall -Wextra)
    if(NOT ANDROID AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86|x64|AMD64")
        target_compile_options(pictor PRIVATE -mavx2)
    endif()
    if(ANDROID AND ANDROID_ABI STREQUAL "arm64-v8a")
        # NEON は arm64-v8a でデフォルト有効、明示不要
    endif()
elseif(MSVC)
    target_compile_options(pictor PRIVATE /W4 /utf-8)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86|x64|AMD64")
        target_compile_options(pictor PRIVATE /arch:AVX2)
    endif()
endif()
```

### 3.4 デモターゲットの Android ゲート

```cmake
if(PICTOR_BUILD_DEMO AND NOT ANDROID)
    add_executable(pictor_benchmark demo/benchmark/main.cpp)
    # …既存
elseif(PICTOR_BUILD_DEMO AND ANDROID)
    # Android では headless demo を shared library (.so) として生やす
    add_library(pictor_mobile_demo_native SHARED demo/mobile/main.cpp)
    target_link_libraries(pictor_mobile_demo_native PRIVATE pictor android log)
endif()
```

---

## 4. 必要な新規コード (Phase 1-2)

### 4.1 `include/pictor/surface/android_surface_provider.h`

```cpp
#pragma once
#include "pictor/surface/surface_provider.h"

struct ANativeWindow;

namespace pictor {

/// Android NDK 用 SurfaceProvider。ホスト (NativeActivity /
/// GameActivity / Jetpack Compose Surface bridge) が生成した
/// ANativeWindow* を受け取り、Pictor に VkSurfaceKHR を作らせる。
class AndroidSurfaceProvider : public ISurfaceProvider {
public:
    AndroidSurfaceProvider(ANativeWindow* window, uint32_t w, uint32_t h);

    NativeWindowHandle get_native_handle() const override;
    SwapchainConfig    get_swapchain_config() const override;
    void               on_swapchain_created(uint32_t width, uint32_t height) override;

    // Android の surfaceChanged コールバックから呼ぶ
    void update_window(ANativeWindow* window, uint32_t w, uint32_t h);

    uint32_t get_required_instance_extensions(const char** out_names, uint32_t max) const override;

private:
    ANativeWindow* window_  = nullptr;
    uint32_t       width_   = 0;
    uint32_t       height_  = 0;
};

} // namespace pictor
```

### 4.2 `src/surface/android_surface_provider.cpp`

- `get_native_handle()` → `NativeWindowHandle::Android` を埋めて返す
- `get_required_instance_extensions()` → `VK_KHR_surface`, `VK_KHR_android_surface` の 2 つ
- lifecycle 連携: `surfaceDestroyed` で `PictorRenderer::on_surface_lost()`、`surfaceCreated` で `on_surface_regained()` を呼ぶのは**ホスト側 JNI** の責務とする (Pictor はフック API を提供、既に実装済)

### 4.3 (Phase 3) JNI ブリッジ `android/`

最低限以下:
- `android/app/build.gradle` — AGP + externalNativeBuild CMake 参照
- `android/app/src/main/cpp/jni_bridge.cpp` — ANativeWindow を取り出して `PictorRenderer` / `AndroidSurfaceProvider` を駆動
- `android/app/src/main/AndroidManifest.xml` — `<uses-feature android:name="android.hardware.vulkan.version" android:version="0x00010001"/>`
- `android/app/src/main/java/.../PictorActivity.kt` — GameActivity or NativeActivity

スコープが膨らむため **Phase 3 で独立 PR** にする。Phase 1-2 は `libpictor.so` が arm64-v8a で生成されるところまで。

---

## 5. ビルド手順 (Phase 1)

### 5.1 環境変数セット

```bash
export ANDROID_NDK_HOME=/opt/android-ndk-r26d
export ANDROID_ABI=arm64-v8a
export ANDROID_API=26
```

### 5.2 CMake クロスビルド (lib 単体)

```bash
cd Pictor
cmake -S . -B build-android-arm64 \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=$ANDROID_ABI \
    -DANDROID_PLATFORM=android-$ANDROID_API \
    -DANDROID_STL=c++_shared \
    -DPICTOR_BUILD_DEMO=OFF \
    -DPICTOR_ENABLE_RIVE=OFF \
    -G Ninja
cmake --build build-android-arm64 -- -j
```

成果物: `build-android-arm64/libpictor.a` (static) もしくは shared に切り替えるなら `libpictor.so`。

### 5.3 x86_64 エミュレータ向け

```bash
cmake -S . -B build-android-x86_64 \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=x86_64 \
    -DANDROID_PLATFORM=android-$ANDROID_API \
    -DPICTOR_BUILD_DEMO=OFF \
    -G Ninja
cmake --build build-android-x86_64 -- -j
```

---

## 6. マイルストーン

### Phase 1 — `libpictor.a` が arm64-v8a でリンク成功 (最優先)

- [ ] CMake の `if(ANDROID)` ブロック追加
- [ ] GLFW / AVX2 / Vulkan find_package の条件分岐
- [ ] `AndroidSurfaceProvider` 実装
- [ ] `memory_subsystem` 等の AVX2 intrinsic を ARM64 向けに fallback
- [ ] `arm64-v8a` + `x86_64` 2 ABI で `cmake --build` 成功

完了条件: 両 ABI で `libpictor.a` が生成される。実機実行は不要。

### Phase 2 — Android Emulator で `pictor_mobile_demo_native` 実行

- [ ] `pictor_mobile_demo` を `add_library(SHARED)` として .so 化
- [ ] 簡易な JNI shim (printf 経由の logcat 出力確認)
- [ ] API 33 x86_64 エミュレータで `adb push` + `adb shell` 起動
- [ ] lifecycle API の pause / resume / thermal を JNI から駆動できる

完了条件: エミュレータ上で logcat に demo の出力が出る。

### Phase 3 — 最小 APK で PictorRenderer を初期化

- [ ] `android/` ディレクトリ + Gradle プロジェクト
- [ ] GameActivity 経由の ANativeWindow 取得
- [ ] Vulkan 初期化 → clear color 1 フレーム描画
- [ ] 実機 (Snapdragon / Mali) で apk install + 起動確認

完了条件: 青い画面が Pixel 実機で表示される。

---

## 7. CI / 自動化 (Phase 2 以降)

- LUDIARS org の self-hosted runner に NDK を置き、`matrix` で Android ABI ビルドを足す
- `LUDIARS/All-In-OneTest` とは分離 (こちらは Pictor 単体の build matrix)
- 成果物の `*.so` を GitHub Artifact に残せば、手元で adb push して確認できる

---

## 8. 未決事項 / 要判断

| 事項 | 選択肢 | 推奨 |
|------|--------|------|
| AndroidSTL | `c++_shared` / `c++_static` | `c++_shared` (AGP 推奨、Rive 等との相性も良い) |
| 最低 API Level | 26 / 28 / 30 / 33 | **26** で出発、Vulkan 1.3 必要なら 33 に引き上げ |
| NEON intrinsic の扱い | `<arm_neon.h>` 使用 / スカラ fallback | まず **スカラ**、ボトルネックが見えたら NEON 化 |
| Rive 対応 | 今やる / 保留 | **保留** (Phase 1-2 は `OFF`) |
| Gradle プロジェクトの場所 | `android/` / 別リポ (LUDIARS/Pictor-Android) | `android/` (Pictor に内包、小さいうちは 1 リポで) |
| Kotlin vs Java | どちらで書く | **Kotlin** (現代 Android 標準、コード量半減) |
| JNI 生成 | 手書き / `bindgen` / `cxx` | 手書き (Pictor の公開 API は小さい) |
| GameActivity vs NativeActivity | どちらを使う | **GameActivity** (Jetpack Games 推奨、キーボード入力等の改善版) |

---

## 9. 関連 Issue / 参照

- Pictor `plan.md` — 全体設計
- `demo/mobile/main.cpp` — MobileLow / MobileHigh プロファイル、lifecycle simulation (本書 Phase 2 の素材)
- `include/pictor/core/mobile_lifecycle.h` — Phase 3 の JNI がここをドライブする
- `src/surface/vulkan_context.cpp` — Android surface 作成パスは既に存在 (L262-273)

---

## 10. 本書の更新ポリシー

- 各 Phase の完了チェックを入れ、ブロッカーが解消されたら該当セクションを更新
- 「未決事項」の項目は判断が入ったら削除、もしくは「決定」欄を追加
- 実装コミット後、実際のコマンドラインや必要な patch を「参照」に差し替える
