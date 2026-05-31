# Pictor セットアップガイド (用途別)

> Pictor (LUDIARS 短縮コード **Pc**) は最下層のレンダリングパイプラインモジュール。
> Pictor 自体は **ライブラリ** (`libpictor` 静的ライブラリ) であり、実行確認は consumer 側の責務 ([[feedback_pictor_no_run]])。
> 本ガイドは **「ビルドと統合」** にフォーカスする。

このフォルダは「○○するためにどうビルドするか」を用途別に引ける索引。
詳細はリンク先に置き、ここでは要点と最短手順だけを示す (DRY。`../../README.md` / `../../CLAUDE.md` の丸写しはしない)。

## 前提

| 項目 | 要件 | 根拠 |
|---|---|---|
| ビルドシステム | **CMake** (3.20 以上) のみ。premake は Pictor 本体では使わない (Rive を prebuilt 消費するだけ) | `../../CMakeLists.txt:1` |
| C++ | C++20 対応コンパイラ (GCC / Clang / MSVC) | `../../CMakeLists.txt:4` |
| CPU | x86/x64 では AVX2 を使用 (ARM64 は `PICTOR_IS_X86` ガードで自動回避) | `../../CMakeLists.txt:271-285` |
| Vulkan SDK | GPU 機能に必要。未検出時は `find_package(Vulkan QUIET)` でスキップされ、headless デモ/テストのみビルド | `../../CMakeLists.txt:46-49` |
| GLFW 3.4 | デスクトップのみ。システム未検出時は FetchContent で自動取得 | `../../CMakeLists.txt:51-72` |

## 最短ビルド (ライブラリのみ)

```bash
cmake -B build -DPICTOR_BUILD_DEMO=OFF
cmake --build build --config Release --target pictor
```

- consumer に組み込むだけなら通常 `add_subdirectory` で取り込むので、単体ビルドは不要 → [`integration.md`](integration.md)。
- **Release 必須の事情** (prebuilt rive_yoga.lib の CRT) は Rive を有効化したときのみ → [`build.md`](build.md) §Release/Debug。

## 用途別インデックス

| 用途 | 設定 | 参照 |
|---|---|---|
| 標準ビルド (generate→build、Debug/Release、依存) | `cmake -B build` → `cmake --build build` | [`build.md`](build.md) |
| ライブラリだけ欲しい (デモ・テスト不要) | `-DPICTOR_BUILD_DEMO=OFF -DPICTOR_BUILD_TESTS=OFF` | [`build-options.md`](build-options.md) |
| ベクター/Rive アニメを使いたい | `-DPICTOR_ENABLE_RIVE=ON -DPICTOR_RIVE_DIR=<path>` (要 prebuilt rive-runtime) | [`build-options.md`](build-options.md) §Rive / [`build.md`](build.md) |
| プロファイラを切りたい | `-DPICTOR_ENABLE_PROFILER=OFF` | [`build-options.md`](build-options.md) |
| ラージページ確保を試したい | `-DPICTOR_USE_LARGE_PAGES=ON` | [`build-options.md`](build-options.md) |
| WebGL2 (ブラウザ) バックエンド | `-DPICTOR_BUILD_WEBGL=ON` (Emscripten) | [`build-options.md`](build-options.md) §WebGL |
| 開発者ツール (feature-selector) を開きたい | `-DPICTOR_BUILD_TOOLS=ON` | [`build-options.md`](build-options.md) |
| ヘッドレステストを回したい | `-DPICTOR_BUILD_TESTS=ON` (既定) → `ctest` | [`build.md`](build.md) §テスト |
| consumer (KS 等) に組み込みたい | `add_subdirectory(Pictor)` + `target_link_libraries(... pictor)` | [`integration.md`](integration.md) |
| Android NDK / iOS でクロスビルド | `PICTOR_MOBILE` 分岐 (GLFW 無効化、NDK Vulkan) | [`../../docs/android-build.md`](../../docs/android-build.md) |

## 関連設計

- ビルドオプション一覧の正本: `../../README.md` の「ビルドオプション」表 + `../../CMakeLists.txt`。
- Rive 統合設計: [[project_pictor_rive]] / `../../cmake/FindRive.cmake`。
- レンダリング拡張 (Visus / カスタムシェーダ): [`../rendering-extensibility-design.md`](../rendering-extensibility-design.md)。
- WebGL2 バックエンド設計: `../../docs/design/webgl_backend.md`。
- モバイル: `../../docs/android-build.md`。
