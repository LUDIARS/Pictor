# Portability — モバイル / Web 対応の実装課題と設計 (総括)

Pictor (C++20 / Vulkan / デスクトップ起点のレンダリング基盤) をデスクトップ以外の
ターゲットへ広げる際の、 現状・課題・設計方針の総括。 詳細は分冊を参照:

- [portability/mobile.md](portability/mobile.md) — Android / iOS
- [portability/web.md](portability/web.md) — Emscripten / WebGL2 / WebGPU

関連 spec: `dx12-backend-design.md` (RHI 抽象 — 設計のみ) /
`metal-backend-design.md` (iOS Metal 直呼び — SHaRC 先行、 MoltenVK 置換) /
`subsystem/webgl.md` (WebGL リファレンス実装) / `subsystem/surface.md`
(ISurfaceProvider) / `subsystem/uma_memory.md` (UMA アップロード方針)。

---

## 1. 現状マトリクス

| 項目 | Desktop (Win/Linux/macOS) | Android | iOS | Web (Emscripten) |
|---|---|---|---|---|
| ビルド分岐 | ✅ 既定 | ✅ CMake 早期分岐 (`CMakeLists.txt:30-36`) | ✅ 分岐あり (`:37-44`) | △ `pictor_webgl` 別ターゲットのみ (`:466-485`)。 本体 lib は wasm 不可 |
| Surface | ✅ GLFW (Win32/X11/Wayland/Cocoa) | ✅ `ANativeWindow` provider + `vkCreateAndroidSurfaceKHR` | ✅ `CAMetalLayer` provider + `vkCreateMetalSurfaceEXT` (Q-2 修正済) | ✅ `emscripten_webgl_create_context` (webgl 側) |
| 描画バックエンド | ✅ Vulkan (ハードコード) | ✅ NDK Vulkan | ✅ MoltenVK (リンクは host 側) | △ WebGL2 リファレンス (icosphere 1 種) |
| SIMD | ✅ AVX2 (x86 のみ) | ❌ NEON なし → スカラ | ❌ 同左 | ❌ wasm SIMD128 なし → スカラ |
| クロスビルド検証 | ✅ (CI: Linux / 手元: MSVC) | ❌ NDK 未検証 | ❌ 未検証 (scaffolding のみ) | ❌ CI に Emscripten なし |
| 実機ライフサイクル | - | △ Controller 実装済 / JNI 配線なし | △ 同左 | - (ブラウザ rAF 前提) |
| テスト | ✅ headless 19 本 | ❌ mobile ビルドはテスト除外 (`:640`) | ❌ 同左 | ❌ なし |

凡例: ✅ 実装・検証済 / △ 部分実装 or 未検証 / ❌ 未実装

## 2. レイヤ横断の共通課題

### 2.1 バックエンド抽象 (RHI) の不在 — 最大の構造課題

Vulkan がコア描画経路にハードコードされている
(`dx12-backend-design.md:3` 「Pictor は現状 Vulkan 専用」、
`rendering-extensibility-design.md:13` 系統B は 「完全ハードコード」)。
露出箇所: `vulkan_context` / `gpu_timer` / `render_pass_scheduler` /
`compiled_graph` / surface providers。 WebGL backend は抽象を介さず
**並置** されており、 `webgl_renderer.h:53-54` の 「RenderBackend trait
(planned)」 は未実装。

**方針**: RHI 導入は dx12-backend-design.md の `IRhiDevice` /
`IRhiCommandEncoder` 案と同一の抽象で解く (DX12 / WebGPU / Metal 直の
将来分岐がすべてここに合流する)。 ただし CLAUDE.md の OOP 境界規約どおり
**抽象は境界 API のみ** とし、 hot path (CompiledGraph 実行) は
バックエンドごとの直値実装を維持する (仮想呼び出しを per-draw に入れない)。
短期 (モバイル) は Vulkan のままで到達可能なため、 RHI は Web/DX12 着手時の
前提課題として扱う。

### 2.2 SIMD 移植性

AVX2 は `#ifdef __AVX2__` + CMake の x86 限定フラグで正しく隔離済み
(`CMakeLists.txt:306-314`、 intrinsics は `update_scheduler.cpp:122-135` の
1 箇所のみ)。 ARM / wasm は自動的にスカラへ落ちる。
**方針**: 「まずスカラ、 ボトルネック計測後に NEON / SIMD128」
(docs/android-build.md:341 の決定を踏襲)。 なお現 AVX2 NT-store 経路自体が
実効なし (review L-3) のため、 移植より先に x86 側の是正が要る。

### 2.3 スレッドモデル

`std::thread` + mutex/condvar (`job_dispatcher.cpp` / `update_scheduler.cpp` /
graph demo の layout thread)。 モバイルはそのまま動くが、 wasm は
`-pthread` + COOP/COEP ヘッダ + Worker pool が前提になる。
**方針**: `IJobDispatcher` 抽象 (実装済) を活かし、 wasm では
シングルスレッド dispatcher (`SingleThreadDispatcher`) へ差し替え可能に
しておく。 スレッド数 0/1 での動作は既にガード済み。

### 2.4 メモリ予算

GPU プール既定 (mesh 256MB + ssbo 128MB + instance/staging ~144MB ≒ 528MB、
`gpu_memory_allocator.h:23-27`) は **プラットフォーム不変**。 モバイル /
wasm では過大。 `DeviceMemoryProfile` (実装・テスト済) は UMA / ReBAR の
判定のみで、 プールサイズに反映されていない。
**方針**: プール構成を `MemoryConfig` 経由でプロファイル
(PipelineProfile) から注入可能にし、 MobileLow 等のプリセットに予算を持たせる。

### 2.5 「正直な未実装」 規約の維持

surface / lifecycle / UMA 判定のように **decision seam だけ先に作り、
実処理は明示スタブ + 警告** とする現行スタイル (§7.1 サイレント失敗禁止) を
移植作業でも守る。 「動いているように見えるスタブ」 は作らない
(review/2026-06-11 D-2 の教訓)。

## 3. 優先順位 (提案)

| 優先 | 項目 | 根拠 |
|---|---|---|
| P0 | Android クロスビルド CI (NDK arm64-v8a、 コンパイルのみ) | 現状 **一度もクロスコンパイルされていない** — 分岐の腐敗を CI で止める |
| P0 | Web 方針の一本化 (C++/Emscripten を正とし Rust 案 doc を整理) | 実装と設計文書が別アーキテクチャを指している |
| P1 | GPU プール予算のプロファイル注入 | モバイル実機で最初に踏む制約 |
| P1 | Android Phase 2 (エミュレータで .so + 最小 JNI) | 実機検証の入口 |
| P2 | UMA Direct アップロードの実コピー実装 | モバイルは全機 UMA — 帯域に直結 |
| P2 | wasm 向け本体ビルド (Vulkan 非依存サブセットの切り出し) | RHI 前提。 web.md §4 参照 |
| P3 | NEON / SIMD128、 iOS Phase、 WebGPU | 計測で必要になってから |
