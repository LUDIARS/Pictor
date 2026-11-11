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

### 6.1 Lorica の取り込みと適用 (`cmake/PictorLorica.cmake`)

- 取り込み: `LORICA_SOURCE_DIR` があれば `add_subdirectory`、無ければ FetchContent で固定 commit。
  取り込む側がすでに `Lorica::boundary` を持つ (自分で `add_subdirectory` / FetchContent / `find_package` した) ときは
  それを使い、Pictor からは何も足さない (二重定義なし)。Lorica の tests / fuzzer / protection は Pictor の
  ビルドでは作らない (通常変数で OFF。取り込む側の cache は FORCE しない)。
- 適用: root の最後の `pictor_lorica_apply_all()` が、Pictor のソース木 (third_party/ と binary dir を除く) の
  全 compiled target に `Lorica::boundary` (PRIVATE `$<BUILD_INTERFACE:...>`)、`lorica_harden_target` (baseline)、
  `lorica_deprecate_unsafe_apis` (Lorica が対応する toolchain のみ) を付ける。その後に足された Pictor の target は
  configure の DEFER 検査で FATAL_ERROR になる。
- 第三者: FetchContent でビルドする `glfw` と `openxr_loader` に baseline を付ける
  (`PICTOR_LORICA_HARDEN_FETCHED_DEPS`、既定 ON。OFF にすると WARNING)。C のため unsafe API の拒否は付けない。
- Lorica が対応しない toolchain では hardening を飛ばし、WARNING で理由と target 数を出す (§4)。
- EXTENDED: 使わない。Pictor には常駐サーバ・ネットワークの受け口・ブリッジの実行ファイルが無い
  (graph demo の clangd channel もホストが渡す値を受けるだけで子プロセスを持たない)。ライブラリは毎フレームの
  描画・カリング・メモリプールを含むので baseline にとどめる。
- MSVC: Lorica の baseline の `/sdl` は C4996 をエラーにするので、Windows で通る `sscanf` / `getenv` / `fopen` を
  置き換えた (`vector_animation.cpp`、`fps_baseline_test`、`pixel_text_test`、fbx_viewer / text / sharc のデモ)。
  MSVC でのビルドは未確認 (§6.5)。
- 取り込む側への影響: Pictor の hardening のフラグ・定義・Lorica の include は consumer に伝わらない
  (取り込む側 3 通りの configure と build.ninja で確認。§6.4)。

### 6.2 入力経路の上限 (`pictor/core/bounded_file.h`、`parse_limits.h`)

| 経路 | 入れたもの | 上限 |
| --- | --- | --- |
| ファイル丸ごと読み (共通) | `read_file_bounded` / `read_text_file_bounded`: 大きさを `lorica::narrow` で確かめ (tellg の -1 を拒否)、上限と比べてから確保 | 呼び出し側の値 |
| FBX `load_file` | 上の関数 | `kMaxModelFileBytes` 512 MiB |
| FBX `parse` (binary) | `lorica::byte_reader` で全読み出し、`lorica::input_budget` で累計予算 (ノード・プロパティの構造体、文字列、展開後の配列を確保の前に計上)、`checked_multiply` で配列の大きさ | `FBXParseLimits`: 2 GiB / 要素 8M / 深さ 256 |
| FBX `parse` (ASCII) | 同じ予算 (ノード・プロパティ・配列の値ごと) | 同上 |
| BVH `import_file` | 上限付きで読む | 512 MiB |
| フォント `load_from_file` | 上限付きで読む | `kMaxFontFileBytes` 256 MiB |
| フォント `hmtx` | numberOfHMetrics を numGlyphs で切る (配列の外への書き込みを修正) | — |
| Rive `load_file` | 上限付きで読む (tellg -1 の巨大 resize と、自分自身からの assign を修正) | `kMaxVectorAnimationFileBytes` 64 MiB |
| Lottie `load_file` | 上限付きで読む | 64 MiB |
| パイプラインプロファイル (hot reload) | 上限付きで読む | `kMaxConfigFileBytes` 16 MiB |
| GI ベイク `load` | ファイルを上限付きで読み、件数 × 各区画の大きさを `checked_multiply` と `byte_reader` で中身と照合してから確保。余りのバイトも拒否 | `kMaxBakeFileBytes` 512 MiB |
| Web mesh 入力 (wasm C ABI) | `vertex_count * 3` と `* 9` を `checked_multiply` (wasm32 の size_t で桁あふれしない) | — |

残り (入れていない。優先の高い順):

1. SPIR-V を読む 10 箇所 (`simple_renderer` `ui_renderer` `ui_overlay_pipeline` `bitmap_text_renderer`
   `gi_gpu_executor` `gi_ssao_compute` `sharc_executor` `decal_system` `postprocess_pipeline` `reprojection_pass`)。
   どれも Vulkan のときだけコンパイルされ、この環境ではビルドできないので触っていない。`read_file_bounded` と
   `shader_registry.cpp` の既存上限 64 MiB に揃えるのが次の作業。
2. `sharc_executor.cpp` の `load_cache` (Vulkan のみ)。
3. デモの入力 (`track_csv` `morph_loader` `obj_mesh` `ply_mesh` `dock_layout`、fbx_viewer の cache)。
4. `fbx_importer` / `fbx_scene` が document から作るメッシュの確保 (document の予算の外)。
5. `vector_animation.cpp` の SVG 文字列 (呼び出し側が渡す長さ)、C API の受け口。

### 6.3 sanitizer と fuzz

- `cmake/PictorSanitizers.cmake`: `PICTOR_SANITIZE` (ASan + UBSan) / `PICTOR_SANITIZE_THREAD` (TSan)。既定 OFF。
- `tests/fuzz/`: 入口 4 つ (`fbx_document` `font_loader` `visus_json` `web_mesh_input`)。どれも回帰用の実行ファイル
  (`pictor_fuzz_regression_*`、seed・その接頭辞・決まった変異を流す) が ctest に入る。`PICTOR_BUILD_FUZZERS=ON`
  (Clang のみ、既定 OFF) で libFuzzer の実行ファイル `pictor_fuzz_*` も作る。

### 6.4 ビルドの手順 (この環境)

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLORICA_SOURCE_DIR=/path/to/Lorica \
      -DGLFW_BUILD_WAYLAND=OFF -DGLFW_BUILD_X11=OFF   # X11/Wayland の開発ヘッダが無い環境だけ
cmake --build build -j2 && ctest --test-dir build
# ASan + UBSan / TSan
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPICTOR_BUILD_DEMO=OFF -DPICTOR_SANITIZE=ON ...
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPICTOR_BUILD_DEMO=OFF -DPICTOR_SANITIZE_THREAD=ON ...
# libFuzzer (Clang)
CC=clang CXX=clang++ cmake -S . -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPICTOR_BUILD_DEMO=OFF \
      -DPICTOR_SANITIZE=ON -DPICTOR_BUILD_FUZZERS=ON \
      -DCMAKE_CXX_FLAGS=-fsanitize=fuzzer-no-link -DCMAKE_C_FLAGS=-fsanitize=fuzzer-no-link ...
./build-fuzz/tests/fuzz/pictor_fuzz_regression_fbx_document --write-seeds corpus
./build-fuzz/tests/fuzz/pictor_fuzz_fbx_document -max_total_time=30 corpus
```

結果の数値は報告 (`/home/user/.cpp-deps/reports/Pictor-report.md`) にある。

### 6.5 人間の判断が要ること (要約)

報告の 9 章を正とする。主なもの: MSVC (Windows) でのビルド確認、`/sdl` が既存コードの警告をエラーにしないか、
守りと速度のどちらを優先するか (計測は目安のみ)、consumer 側の TU と Pictor の TU で標準ライブラリの assertions /
FORTIFY の設定が違うこと、`cmake/PictorLorica.cmake` とこの文書のドメイン登録 (release-metadata は lockedFields)。

### 6.6 結果 (2026-11-11、この環境)

- GCC 13 Release: 着手時 ctest 52/52 → 最後 ctest 57/57 (C++ 56 + Node 1)。警告の数は着手時と同じ。
- ASan + UBSan / TSan (RelWithDebInfo、`-O1 -g1`、デモなし): どちらも ctest 57/57
  (`fps_baseline_test` は sanitizer では既定の 20 ms 予算を超えるので `PICTOR_FPS_THRESHOLD_MS=500`)。
  sanitizer で見つけて直したもの: `unit_frame_tap_test` の配列外読み (ASan)、`fps_baseline_test` の callback の
  データ競合 (TSan)。
- libFuzzer (Clang 18、ASan + UBSan、各 30 秒): font_loader (15,417 runs)・visus_json (1,306,125)・web_mesh_input
  (8,120,223。この入口のコードが同じ一つ前の fuzz ビルドでの値) は所見なし。修正後の fbx_document は 998,342 runs で所見なし。fbx_document で 2 件
  (長さ 0 の配列の memcpy、ASCII の巨大な double の int64 への変換。どちらも UBSan) を直し、seed とテストに入れた。
- 守り: `compile_commands.json` の全件に Lorica の baseline のフラグ、打ち消す設定 0 件。`verify_binary.py` で
  build ツリーの ELF 実行ファイルすべてが PIE / NX stack / full RELRO。
- 速度 (目安): `fps_baseline_test` の平均フレーム時間は着手時 4.48 ms → Lorica 適用後 4.56 ms (+2% 前後、
  混んだ機械でのばらつきの範囲)。

### 6.7 「Lorica を使っているか」のテストと CI での取得 (2026-11-11 追記)

- `tests/lorica_applied/`: `lorica_applied_tu_test` (強制 include された `unsafe_api.hpp` の宣言・`__SSP_STRONG__`・`__CET__`・PIE・`_GLIBCXX_ASSERTIONS` / `_LIBCPP_HARDENING_MODE`・最適化ビルドの FORTIFY 3 を TU の中から確かめ、`checked_add` / `input_budget` を実行)、`lorica_applied_compile_commands_test` (`cmake -P` で `compile_commands.json` の Pictor の全 TU と source-built GLFW の C を点検。Visual Studio / Xcode・守りなしの toolchain は `PICTOR_LORICA_SKIP:` で skip)、`lorica_applied_binary_<target>_test` (python3 と readelf があるときだけ、tests/ の外の実行ファイルと probe に `verify_binary.py`)。期待値は toolchain から決め、Lorica が付けた property は読まない。
- わざと壊した確認: target を Lorica から外す (`PICTOR_LORICA_APPLIED` だけ立てる)、`-U_GLIBCXX_ASSERTIONS` / `-fno-stack-protector` / `-fcf-protection=none` / `-D_FORTIFY_SOURCE=0` / `-Wno-deprecated-declarations` を後から足す、強制 include を外す、のどれでも上のテストが落ちる。
- `CMAKE_EXPORT_COMPILE_COMMANDS` は top-level のときだけ ON。CI (`build.yml`) は `cmake/PictorLorica.cmake` の固定 SHA を読み、`LUDIARS_REPO_TOKEN` で `LUDIARS/Lorica` を `_ci/lorica` に取得して `LORICA_SOURCE_DIR` に渡す (secret が空なら `::error::` で止める)。ソースツリー内の Lorica は Pictor の target として数えない。

### 6.8 CMake の最低版を 3.24 に上げた (2026-11-11 追記)

- Lorica の CMakeLists.txt が 3.24 を要求し、3.20〜3.23 では Lorica の取り込みで止まっていたため、Pictor 自身の宣言 (`CMakeLists.txt`・`demo/android/CMakeLists.txt`・`tests/lorica_applied/check_compile_commands.cmake`) と README / spec / docs の記述を 3.24 に揃えた。古い CMake には対応しない (依頼者の判断)。CMake 3.20.5 では `CMakeLists.txt:1` の「CMake 3.24 or higher is required」で止まる。
- 3.21〜3.24 の policy (CMP0126〜CMP0139) が NEW になる影響は、GCC Release の既定構成と Clang の ASan + UBSan + libFuzzer 構成で、変更前後の `compile_commands.json`・`build.ninja`・`rules.ninja` (GCC は `CMakeCache.txt` も) を比べて差なし。Pictor 自身の CMake は該当 policy の対象の命令 (try_compile・CheckIPOSupported・if(PATH_EQUAL) など) を使っていない。`set(... CACHE ...)` (CMP0126) は、取り込む側が同名の通常変数 (`LORICA_SOURCE_DIR` など) を先に置いたとき、それを消さずに使う形に変わる (意図どおり)。MSVC・Android NDK・Emscripten の構成では比べていない。
