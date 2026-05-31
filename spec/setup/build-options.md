# ビルドオプション / define / 環境変数

`../../CMakeLists.txt` で実在を確認したものだけを列挙する。手順は [`build.md`](build.md)、索引は [`README.md`](README.md)。

## CMake option (`-D<NAME>=ON/OFF`)

| オプション | 既定 | 効果 | 例 / 根拠 |
|---|---|---|---|
| `PICTOR_BUILD_DEMO` | `ON` | デモ + ベンチマーク群をビルド。ウィンドウデモは Vulkan + GLFW 必須 | `-DPICTOR_BUILD_DEMO=OFF` / `:8` |
| `PICTOR_BUILD_TOOLS` | `OFF` | 開発者ツール target (feature-selector 等) を有効化。`pictor` ライブラリサイズには影響しない | `-DPICTOR_BUILD_TOOLS=ON` / `:9,573` |
| `PICTOR_BUILD_TESTS` | `ON` | headless テストスイート (CTest) をビルド (非モバイルのみ) | `-DPICTOR_BUILD_TESTS=OFF` / `:10,557` |
| `PICTOR_ENABLE_PROFILER` | `ON` | 組込みプロファイラを有効化 → define `PICTOR_PROFILER_ENABLED=1` | `-DPICTOR_ENABLE_PROFILER=OFF` / `:11,246` |
| `PICTOR_USE_LARGE_PAGES` | `OFF` | ラージページ確保を有効化 → define `PICTOR_LARGE_PAGES=1` (PRIVATE) | `-DPICTOR_USE_LARGE_PAGES=ON` / `:12,250` |
| `PICTOR_BUILD_WEBGL` | `OFF` | WebGL2 バックエンド (別ライブラリ `pictor_webgl`) をビルド。Emscripten 前提 | `-DPICTOR_BUILD_WEBGL=ON` / `:13,390` |
| `PICTOR_ENABLE_RIVE` | `OFF` | Rive Renderer 統合。prebuilt rive-runtime が必要 (下記 §Rive) | `-DPICTOR_ENABLE_RIVE=ON` / `:14,260` |

> 注: `../../README.md` には `PICTOR_BUILD_C_API` (C ABI エクスポート) が列挙されているが、現行 `../../CMakeLists.txt` には対応する `option()` もターゲット定義も無く、ビルドフラグとしては未配線。`include/pictor/c_api.h` / `src/c_api/c_api.cpp` のソースは存在するが、現状このフラグを渡しても効果は無い。同様に consumer (KS) や CI が渡す `PICTOR_BUILD_BENCHMARK` も Pictor 側では未配線で、無害なキャッシュ変数として無視される。本ガイドでは未実装のため操作対象に含めない。

## Rive 統合の入力 (§Rive)

`PICTOR_ENABLE_RIVE=ON` のとき `find_package(Rive REQUIRED)` が走り、`Rive::Rive` をリンクして `PICTOR_HAS_RIVE=1` を define する (`../../CMakeLists.txt:260-265`)。`cmake/FindRive.cmake` が参照する入力:

| 変数 | 既定 | 効果 | 根拠 |
|---|---|---|---|
| `PICTOR_RIVE_DIR` | (未設定) | rive-runtime チェックアウトのルート。`renderer/out/<config>/` に compiled static lib がある想定。未設定なら `Rive_FOUND=FALSE` | `../../cmake/FindRive.cmake:11-14,36-39` |
| `PICTOR_RIVE_CONFIG` | `release` | シングルコンフィグ生成系での構成フォールバック (`release` / `debug`)。マルチコンフィグ生成系では両方を探索し per-config マッピング | `../../cmake/FindRive.cmake:15-22,41-43` |

prebuilt のビルド手順は [`build.md`](build.md) §4 を参照。**Rive 有効時は Release 必須** (rive_yoga.lib が Release 専用 CRT、Debug は LNK2038/LNK1319) ([[feedback_ks_release_build_required]])。

## コンパイル define (Pictor が立てるもの)

option やプラットフォーム検出から自動で立つ define。consumer が手で渡すものではないが、条件付きコードを読むときの参照用。

| define | 立つ条件 | スコープ | 根拠 |
|---|---|---|---|
| `PICTOR_HAS_VULKAN=1` | `Vulkan_FOUND` | PUBLIC | `:220` |
| `PICTOR_PROFILER_ENABLED=1` | `PICTOR_ENABLE_PROFILER=ON` | PUBLIC | `:247` |
| `PICTOR_LARGE_PAGES=1` | `PICTOR_USE_LARGE_PAGES=ON` | PRIVATE | `:251` |
| `PICTOR_HAS_RIVE=1` | `PICTOR_ENABLE_RIVE=ON` | PUBLIC | `:263` |
| `PICTOR_HAS_WEBGL=1` | `pictor_webgl` ビルド時 | PUBLIC (pictor_webgl) | `:404` |
| `VK_USE_PLATFORM_WIN32_KHR=1` / `NOMINMAX` | Win32 + Vulkan | PUBLIC | `:237` |
| `VK_USE_PLATFORM_XLIB_KHR=1` | Linux + Vulkan | PUBLIC | `:239` |
| `VK_USE_PLATFORM_MACOS_MVK=1` | macOS + Vulkan | PUBLIC | `:241` |
| `VK_USE_PLATFORM_ANDROID_KHR=1` | Android | PUBLIC | `:224` |
| `VK_USE_PLATFORM_METAL_EXT=1` | iOS (MoltenVK) | PUBLIC | `:233` |

PUBLIC define は `pictor` をリンクした consumer にも伝播する。

## コンパイルオプション (コンパイラ別)

| 環境 | 付与されるフラグ | 条件 | 根拠 |
|---|---|---|---|
| GCC / Clang | `-Wall -Wextra` | 常時 | `:276` |
| GCC / Clang | `-mavx2` | x86/x64 かつ非モバイル | `:277-279` |
| MSVC | `/W4 /utf-8` | 常時 (`pictor` への PRIVATE) | `:281` |
| MSVC | `/arch:AVX2` | x86/x64 | `:282-284` |

### MSVC `/utf-8` は伝播しない (重要)

`/utf-8` は `pictor` への **PRIVATE** 指定なので、consumer / テストターゲットには伝播しない。日本語コメントを含む Pictor の header を取り込む側 (テスト / consumer) は **自前で `/utf-8` を付ける**必要がある ([[feedback_msvc_utf8_test_targets]])。テスト側の実例は `../../tests/CMakeLists.txt:30-33`、consumer 側の付け方は [`integration.md`](integration.md) を参照。

## WebGL2 バックエンド (§WebGL)

`PICTOR_BUILD_WEBGL=ON` または `EMSCRIPTEN` のとき、別ライブラリ `pictor_webgl` をビルドする (`../../CMakeLists.txt:390-409`)。

- Emscripten 環境では `-sUSE_WEBGL2=1` / `-sFULL_ES3=1` が付与される (`:407-408`)。
- 設計の詳細: `../../docs/design/webgl_backend.md`。

## モバイル (Android / iOS)

`ANDROID` / `IOS` 検出で `PICTOR_MOBILE=ON` となり、GLFW FetchContent とデスクトップ surface provider をスキップ、NDK 同梱 Vulkan / MoltenVK を直接指す (`../../CMakeLists.txt:28-44,206-212`)。AVX2 フラグは `PICTOR_IS_X86` ガードで ARM64 では付かない (`:271-285`)。詳細は `../../docs/android-build.md`。
