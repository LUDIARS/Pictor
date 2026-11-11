# Lorica 導入 (job cpp-lorica-20261111) — 棚卸しと進め方

Pictor に防御ライブラリ Lorica (LUDIARS/Lorica、固定 `5e8281b0e60fa16f814e16eaa620715937e8618d`) を入れる作業の
棚卸し・決めたこと・残りをまとめる。次にこの作業を続ける人はここから読む。
Lorica は「守りの層を足す」もので、Pictor がメモリ安全になったという意味ではない。

## 1. ビルド単位 (CMake target) と作る条件

Linux 列: この環境 (Linux x86_64、GCC 13.3 / Clang 18、Vulkan SDK・GPU・窓なし) で configure・ビルドできたか。

| 区分 | target | 作る条件 | Linux |
| --- | --- | --- | --- |
| ライブラリ | `pictor_core` | 常に (Emscripten 含む) | ビルド可 |
| ライブラリ | `pictor` | Emscripten 以外 | ビルド可 (GLFW を FetchContent。下の「GLFW」参照) |
| ライブラリ | `pictor_postprocess` | `PICTOR_ENABLE_POSTPROCESS` (既定 ON) | ビルド可 |
| ライブラリ | `pictor_decals` | `PICTOR_ENABLE_DECALS` (既定 ON) | ビルド可 |
| ライブラリ | `pictor_xr` | `PICTOR_ENABLE_XR` (既定 ON)。OpenXR backend は `PICTOR_ENABLE_OPENXR` + Vulkan | ビルド可 (OpenXR なし) |
| ライブラリ | `pictor_sharc_vulkan` | `PICTOR_ENABLE_SHARC_VULKAN` (既定 = Vulkan の有無) | 不可 (Vulkan SDK なし) |
| ライブラリ | `pictor_sharc_dx12` | `PICTOR_ENABLE_SHARC_DX12` (既定 = WIN32) | 不可 (Windows 専用) |
| ライブラリ | `pictor_polynomial_demo` | `PICTOR_BUILD_POLYNOMIAL_SDK` + Windows + Vulkan | 不可 (Windows 専用) |
| ライブラリ | `pictor_webgl` | `PICTOR_BUILD_WEBGL` または Emscripten | 未確認 (emsdk なし。native の `PICTOR_BUILD_WEBGL=ON` は WebGL/GLES ヘッダが要る) |
| 検査 | `pictor_gfx_header_check` (OBJECT) | `PICTOR_BUILD_GFX_HEADER_CHECK` (既定 ON) | ビルド可 |
| ツール | `visus_migrate` | (`PICTOR_BUILD_TOOLS` または `PICTOR_BUILD_TESTS`)、モバイル・WEBGL 以外 | ビルド可 |
| デモ (headless) | `pictor_benchmark` `pictor_fbx_demo` `pictor_text_effects_demo` `pictor_material_serializer_demo` | `PICTOR_BUILD_DEMO` (既定 ON)、デスクトップ | ビルド可 (起動はしない) |
| デモ (窓・Vulkan) | `pictor_demo` `pictor_text_demo` `pictor_rive_demo` `pictor_graph_demo` `pictor_fbx_viewer` `pictor_pn_demo` | `PICTOR_BUILD_DEMO` + Vulkan + GLFW | 不可 (Vulkan SDK なし) |
| デモ | `pictor_sharc_demo` | 上 + `pictor_sharc_vulkan` | 不可 |
| デモ | `pictor_vr_openxr_demo` | 上 + `PICTOR_ENABLE_OPENXR` | 不可 |
| デモ | `pictor_sharc_dx12_test` | `pictor_sharc_dx12` (Windows) | 不可 |
| デモ (wasm) | `pictor_webgl_demo` `pictor_web_mesh` | Emscripten (`pictor_web_mesh` は `web-mesh/` の ES module + wasm) | 不可 (emsdk なし) |
| テスト | `tests/` の 47 本 (`pictor` にリンク。うち postprocess 3・decal 1・xr 3・sharc 2 は option 条件) | `PICTOR_BUILD_TESTS` (既定 ON)、モバイル以外 | ビルド可 |
| テスト | `tests/` の 4 本 (`pictor_core` だけにリンク) | 同上 (`unit_null_backend_test` は `PICTOR_RHI_NULL`) | ビルド可 |
| テスト (Node) | `web_mesh_js_test` (`web-mesh/test/*.test.mjs`) | node がある | 実行可 (C++ でない。Lorica の対象外) |
| 別プロジェクト | `demo/android/CMakeLists.txt` の `pictor_demos` (SHARED) | Android NDK (Pictor を `add_subdirectory` する側) | 不可 (NDK なし) |

既定の Linux configure で作られる Pictor 自身の compiled target は 62 (ライブラリ 5、検査 1、ツール 1、headless デモ 4、
テスト 51)。Linux でビルドできない Pictor 自身の target は 15 (`pictor_sharc_vulkan` `pictor_sharc_dx12`
`pictor_polynomial_demo` `pictor_webgl` `pictor_webgl_demo` `pictor_web_mesh` と窓・Vulkan・Windows のデモ 9)。

`tests/` に置いてあるが CMake に登録されていないテストのソース: `unit_cache_info_test.cpp`
`unit_device_memory_profile_test.cpp` `unit_perf_introspection_test.cpp` (この job では触らない。報告にだけ書く)。

## 2. 第三者のコード

| 名前 | 形 | 扱い |
| --- | --- | --- |
| GLFW 3.4 | システムに無ければ FetchContent でソースからビルド (C) | Pictor から `lorica_harden_target(glfw)` で baseline を差し込む (ソースは書き換えない)。C なので unsafe API の拒否は対象外。システムの glfw3 (imported) は守れない |
| OpenXR-SDK loader | `PICTOR_ENABLE_OPENXR` で、インストール済みが無ければ FetchContent | 同じく `openxr_loader` に baseline を差し込む記述を入れた。この環境では Vulkan が無く configure できないので未確認 |
| stb_image (`third_party/stb`) | ヘッダ + `stb_image_impl.cpp` をデモの target に直接コンパイル | デモ target の一部として baseline・unsafe API の拒否がかかる。ソースは書き換えない |
| rive-runtime | ビルド済み (`cmake/FindRive.cmake`、`PICTOR_ENABLE_RIVE`) | ビルド済みなので守れない |
| Intel PCM | ビルド済み (`PICTOR_ENABLE_HW_COUNTERS`) | ビルド済みなので守れない |
| Vulkan SDK / D3D12 / Metal / Emscripten の WebGL2 | システム | 対象外 |

## 3. 外から入る入力の経路

上限の有無は着手時点 (`a6b84d7`) のもの。この job で入れたものは §6 に書く。

| 経路 | 場所 | 着手時の上限 |
| --- | --- | --- |
| FBX (binary / ASCII) | `src/animation/fbx_document.cpp` (`load_file` / `parse`) | 深さ 256、配列 1 個の展開 256 MiB。**ファイルの大きさ・ノードとプロパティの数・展開の累計に上限なし** (1 バイトのプロパティ型コードが ~170 バイトの `FBXProperty` になる増幅) |
| FBX → シーン | `fbx_importer.cpp` `fbx_scene.cpp` | document の内容に従う |
| BVH | `src/animation/bvh_importer.cpp` | ファイルの大きさに上限なし |
| フォント (TTF / OTF / TTC) | `src/text/font_loader.cpp` | cmap 展開 0x110000。**ファイルの大きさに上限なし。`hmtx` で numberOfHMetrics > numGlyphs のとき配列の外へ書く** |
| TrueType アウトライン | `src/text/truetype_outline.cpp` | span の境界検査・点 262144・合成 1024・深さ 16 (既存で十分) |
| Rive (.riv) | `src/animation/rive_animation.cpp` `src/vector/rive_renderer.cpp` | 上限なし。`tellg()` が -1 のとき巨大な resize |
| Lottie (JSON) | `src/animation/lottie_animation.cpp` | 上限なし |
| SVG (文字列) | `src/animation/vector_animation.cpp` | 上限なし (呼び出し側が渡す文字列) |
| Visus JSON / パッケージ | `src/visus/visus_json.cpp` ほか | 16 MiB・深さ・値の数 (既存) |
| Visus リソース | `src/visus/resource_loader.cpp` | 設定の `max_file_bytes`・ルート外の拒否 (既存) |
| パイプラインプロファイル JSON (hot reload で読み続ける) | `src/pipeline/pipeline_profile_serializer.cpp` `pipeline_hot_reload.cpp` | ファイルの大きさに上限なし |
| マテリアル JSON | `src/material/material_serializer.cpp` | 呼び出し側の文字列 |
| SPIR-V シェーダ | `simple_renderer` `ui_renderer` `ui_overlay_pipeline` `bitmap_text_renderer` `gi_gpu_executor` `gi_ssao_compute` `sharc_executor` `decal_system` `postprocess_pipeline` `reprojection_pass` `shader_registry` | 多くは上限なし |
| GI ベイク結果 | `src/gi/gi_bake.cpp` `GIBakeSystem::load` | **ファイルの件数をそのまま resize (最大 4G 件)** |
| SHaRC キャッシュ | `src/gi/sharc_executor.cpp` `load_cache` | ヘッダを設定と照合 (Vulkan のみ) |
| 環境変数 | `PICTOR_FRAME_TAP` (`frame_tap_config.cpp`)、テストの `PICTOR_FPS_THRESHOLD_MS` | 値の検証あり |
| sysfs | `src/core/cache_info.cpp` | 数値 1 個 |
| C API | `src/c_api/c_api.cpp` | ホストのプロセス内の呼び出し |
| Web mesh (wasm の C ABI) | `src/webgl/web_mesh_exports.cpp` → `web_mesh_input.cpp` | 値の検証あり。**wasm32 では `vertex_count * 3` が `size_t` (32 bit) で桁あふれしうる** |
| デモの入力 | `demo/fbx_viewer/track_csv.cpp` `morph_loader.cpp` (cache)、`demo/sharc/obj_mesh.cpp` `ply_mesh.cpp`、`demo/text/main.cpp`、`demo/graph/dock_layout.cpp`、stb_image | デモごと。大半が Linux でビルドできない |
| ツール | `tools/visus_migrate` (引数 + Visus JSON) | 引数の表示を無害化、本体は Visus の上限 |
| JS / wasm の受け口 (Lorica の対象外) | `browser/` (npm パッケージ、WebGL2 で mesh を描く JS)、`web-mesh/` (wasm の JS 面)、`tools/level-editor/serve.mjs` (Node のローカル HTTP サーバ)、`tools/frame_tap/validate-render-tap.mjs` | C++ でないので一覧に挙げるだけ |

## 4. 対応 OS・ツールチェインと Lorica

| 環境 | Pictor のビルド | Lorica の守り |
| --- | --- | --- |
| Windows MSVC (x64) | 主な利用先 (Ergo・ゲーム) | baseline (`/GS /sdl /guard:cf` など)・unsafe API の拒否 (`/we4996`)。この環境では未確認 |
| Windows clang-cl | — | baseline のみ (Lorica は clang-cl の unsafe API の拒否に対応しない) |
| Linux GCC / Clang | CI (ubuntu) | baseline・unsafe API の拒否 (glibc 2.39 の Clang も可) |
| Emscripten (WebGL2・web-mesh) | `pictor_core` `pictor_webgl` `pictor_webgl_demo` `pictor_web_mesh` | **適用しない** (configure に WARNING)。Lorica::boundary の検査だけ入る |
| Android NDK | `demo/android` から `add_subdirectory` | **適用しない** (CMAKE_SYSTEM_NAME が Android。WARNING) |
| iOS (Metal) | `pictor` | **適用しない** (WARNING) |
| macOS (MoltenVK) | `pictor` | **適用しない** (WARNING) |
| MinGW | — | **適用しない** (WARNING) |

## 5. 着手時の状態 (`a6b84d7`)

- configure: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGLFW_BUILD_WAYLAND=OFF -DGLFW_BUILD_X11=OFF`。
  GLFW 3.4 の既定 (Wayland + X11) は、この環境に `wayland-scanner` と `libxrandr-dev` `libxinerama-dev` `libxcursor-dev`
  `libxi-dev` (X11 拡張のヘッダ。GLFW は実行時に dlopen するのでヘッダだけで足りる) が無く configure が止まる。
  パッケージは入れず、GLFW の null platform だけを作る option で通した (ローカルの configure だけ。コミットする CMake は変えない)。
- Vulkan SDK が無いので、Vulkan を要るデモ・SHaRC Vulkan は作られない (CMake の既存の分岐どおり)。
- ビルド: 298 ステップ通過。ctest: 52 本すべて通過 (C++ 51 + Node 1)。

## 6. やったこと・残り

報告 `/home/user/.cpp-deps/reports/Pictor-report.md` と同じ内容を、段階ごとにここへ追記する。
