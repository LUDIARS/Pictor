# 標準ビルド手順

Pictor は CMake プロジェクト (`../../CMakeLists.txt`)。出力は静的ライブラリ `libpictor` (`add_library(pictor STATIC ...)`、`../../CMakeLists.txt:74`) と、オプションのデモ群。

オプション一覧は [`build-options.md`](build-options.md)、consumer 組込みは [`integration.md`](integration.md) を参照。

## 1. generate → build

```bash
# generate (構成生成)
cmake -B build

# build (Release を明示)
cmake --build build --config Release
```

- 既定で `PICTOR_BUILD_DEMO=ON` / `PICTOR_BUILD_TESTS=ON` / `PICTOR_ENABLE_PROFILER=ON` (`../../CMakeLists.txt:8-11`)。
- ライブラリだけ欲しいときは `cmake -B build -DPICTOR_BUILD_DEMO=OFF` 後に `--target pictor`。
- マルチコンフィグ生成系 (Visual Studio) は `--config` で、シングルコンフィグ生成系 (Ninja / Unix Makefiles) は generate 時に `-DCMAKE_BUILD_TYPE=Release` で構成を指定する。

### CI と同じ最小構成 (参考)

CI (`../../.github/workflows/build.yml`) は Ninja + Release で `pictor` ターゲットのみをビルドする:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICTOR_BUILD_DEMO=OFF
cmake --build build --target pictor --parallel
```

## 2. 前提依存

| 依存 | 必須/任意 | 挙動 |
|---|---|---|
| Vulkan SDK | 任意 | あれば `PICTOR_HAS_VULKAN=1` が立ち GPU 機能とウィンドウデモが有効。無ければ headless デモ/テストのみ (`../../CMakeLists.txt:46-49,219-244`) |
| GLFW 3.4 | 任意 (デスクトップ) | システム未検出時は FetchContent で `3.4` を自動取得 (`../../CMakeLists.txt:51-72`) |
| glslc (Vulkan SDK 同梱) | デモ時のみ | `PICTOR_BUILD_DEMO` かつ `Vulkan_GLSLC_EXECUTABLE` 検出時に SPIR-V を焼く (`../../CMakeLists.txt:437`) |

ウィンドウを開くデモ (`pictor_demo` / `pictor_graphics_demo` 等) は **Vulkan + GLFW が両方揃ったときだけ** add_executable される (`../../CMakeLists.txt:335,344`)。揃わない場合は `pictor_benchmark` などの headless デモのみ。

## 3. Release / Debug の別

| 構成 | 使いどころ | 注意 |
|---|---|---|
| Debug | 普段の確認の既定。診断ログは Debug でしか出ない ([[feedback_pictor_debug_default]]) | Rive 有効時は後述の制約あり |
| Release | CI / 配布 / **Rive 有効時** | — |

### Rive 有効時は Release ビルド必須

`PICTOR_ENABLE_RIVE=ON` で消費する **prebuilt `rive_yoga.lib` (および rive-runtime の各 static lib) は Release 専用 CRT** でビルドされている。そのため Pictor 側を Debug でリンクすると CRT 不一致で **LNK2038 / LNK1319** になる ([[feedback_ks_release_build_required]])。

- Rive を使うときは **Release でビルドする**。
- Debug でリンクしたい場合は、rive-runtime 側を debug CRT で別途ビルドして与える必要がある。`FindRive.cmake` はマルチコンフィグ生成系向けに debug/release 両方を探索し per-config でマッピングするので、両構成を用意できるなら Visual Studio 生成系で両対応できる (`../../cmake/FindRive.cmake:19-34,182-207`)。
- 注意: コンパイル自体は通ってしまい **リンク段階で初めて失敗する** ため、「ビルドが進んでいるのに最後で落ちる」症状になりやすい。

> Rive を使わない (`PICTOR_ENABLE_RIVE=OFF`、既定) ならこの制約は無く、Debug/Release どちらでもビルドできる。

## 4. prebuilt 連携 (Rive)

Rive Renderer は premake5 ベースの独自ビルドを持つため、Pictor は **prebuilt static library として外部消費**する (`../../cmake/FindRive.cmake:1-9`)。手順:

```bash
git clone https://github.com/rive-app/rive-runtime
cd rive-runtime/renderer
# Windows (Git Bash) — Release / Debug を必要に応じて
../build/build_rive.bat release --with_vulkan --toolset=msc --windows_runtime=dynamic_release
../build/build_rive.bat debug   --with_vulkan --toolset=msc --windows_runtime=dynamic_debug
```

その後 Pictor を:

```bash
cmake -B build -DPICTOR_ENABLE_RIVE=ON -DPICTOR_RIVE_DIR=/path/to/rive-runtime
cmake --build build --config Release
```

`FindRive.cmake` は `renderer/out/<config>/` (または `out/<config>/`) の static lib を探索し、`Rive::Rive` imported target として `rive_renderer` / `rive` / `rive_decoders` / harfbuzz / sheenbidi / yoga / 画像ライブラリをリンク順込みで束ねる (`../../cmake/FindRive.cmake:48-55,116-125,158-172`)。詳細フラグは [`build-options.md`](build-options.md) §Rive。

## 5. テスト

ヘッドレステスト (CTest) は `PICTOR_BUILD_TESTS=ON` (既定) かつ非モバイルで有効 (`../../CMakeLists.txt:557`)。

```bash
cmake -B build            # PICTOR_BUILD_TESTS は既定 ON
cmake --build build
ctest --test-dir build
```

- テストは GLFW / Vulkan ウィンドウ不要の headless で、`pictor` 静的ライブラリのみに依存する (`../../tests/CMakeLists.txt:1-9`)。
- **MSVC では各テストターゲットに `/utf-8` が個別付与される** (`../../tests/CMakeLists.txt:30-33`)。`target_compile_options(pictor PRIVATE /utf-8)` は consumer/テストへ伝播しないため、日本語コメントを含む header を Shift-JIS 誤読させない措置 ([[feedback_msvc_utf8_test_targets]])。

## 6. 出力物

- `libpictor` — 静的ライブラリ (consumer がリンクする本体)。
- `pictor_benchmark` / `pictor_fbx_demo` / `pictor_mobile_demo` / `pictor_text_effects_demo` / `pictor_material_serializer_demo` — headless デモ (Vulkan/GLFW 不要、`PICTOR_BUILD_DEMO=ON` 時、`../../CMakeLists.txt:298-322`)。
- `pictor_demo` / `pictor_graphics_demo` / `pictor_ocean_demo` / `pictor_text_demo` / `pictor_postprocess_demo` / `pictor_rive_demo` / `pictor_texture2d_demo` / `pictor_fbx_viewer` — Vulkan + GLFW 必須のデモ (`../../CMakeLists.txt:335-380`)。

> Pictor の方針として、本リポでの作業は **ビルドまで**。`.exe` の起動・実行確認は consumer/ユーザー側 ([[feedback_pictor_no_run]])。
