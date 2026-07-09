# Portability — Web (Emscripten / WebGL2 / WebGPU)

Web 対応の現状・実装課題・設計。 総括は [../portability.md](../portability.md)。
WebGL バックエンド自体の仕様は `spec/feature/subsystem/webgl.md`。

---

## 1. 現状 (実装済みの範囲)

### `pictor_webgl` — 独立リファレンスターゲット (実装済み)

- `CMakeLists.txt:466-485`: `PICTOR_BUILD_WEBGL OR EMSCRIPTEN` で
  **本体 `pictor` とは別の** static lib `pictor_webgl` をビルド。 ソースは
  `core/types.cpp` + `src/webgl/` の 4 ファイルのみ。 共有コードは types だけ。
- 中身は実装 (スタブではない):
  - `webgl_context.cpp` — `emscripten_webgl_create_context` / HiDPI /
    能力クエリ / 拡張 probe (`EXT_color_buffer_float` 等) / depth・cull・blend 状態
  - `webgl_shader.cpp` — compile/link + uniform 位置キャッシュ + UBO binding
  - `webgl_buffer.cpp` — VBO/IBO/UBO/VAO 一式
  - `webgl_renderer.cpp` — icosphere 1 メッシュの instanced 描画
    (80B/instance、 CPU frustum cull、 GLSL ES 3.00 内蔵)
- demo は Emscripten 時のみ `.html` 出力 (`:488-509`)。
- `spec/feature/subsystem/webgl.md` は実装と **一致** (「共通抽象を持たない
  最小リファレンス」 と自己申告)。

### 本体ライブラリは wasm 不可 (事実)

`add_library(pictor ...)` に Emscripten ガードは無く (`CMakeLists.txt:75`)、
Vulkan をリンクし (`:248`)、 umbrella `pictor.h:65` が `vulkan_context.h` を
無条件 include する。 つまり **Pictor 本体 (SoA scene / culling / batch /
text / animation) は現状 wasm でビルドできない**。

### 周辺の web 系資産 — 3 系統が不整合

| 系統 | 実体 | 状態 |
|---|---|---|
| (a) C++ / Emscripten `pictor_webgl` | `src/webgl/` | **実装済み** (上記) |
| (b) Rust + wasm-bindgen 設計 | `docs/design/webgl_backend.md` | **文書のみ**。 `*.rs` / `Cargo.toml` は repo に存在しない。 実装 (a) と別アーキテクチャを記述しており、 チェックリストは全項目未着手 |
| (c) level editor (純 JS) | `tools/level-editor/` | 独立プロトタイプ。 Pictor 本体と未接続 (「native モデルデータへの置換は将来」 と README 明記) |

WebGPU はコード 0 行 (文書内の将来言及のみ)。 `RenderBackend` / RHI 抽象も
未実装 (`webgl_renderer.h:53-54` の "planned"、 `dx12-backend-design.md`)。

## 2. 実装課題 (ギャップ一覧)

| # | 課題 | 事実 (根拠) | 影響 |
|---|---|---|---|
| W-P1 | **設計文書と実装の乖離** | (b) Rust 案 vs (a) C++ 実装 | 着手者がどちらを正とすべきか判断できない |
| W-P2 | **RHI 抽象の不在** | Vulkan がコア経路にハードコード (`dx12-backend-design.md:3`) | 本体機能 (batch/text/post) を Web へ持ち出す道が無い |
| W-P3 | **本体の wasm ビルド不能** | `pictor.h` → vulkan_context 無条件 include、 CMake ガードなし | scene/culling/batch 等の Vulkan 非依存部すら Web で再利用できない |
| W-P4 | **スレッド前提** | `std::thread` (job_dispatcher / update_scheduler / graph demo layout) | wasm は `-pthread` + COOP/COEP が必要。 未対応ページでは動かない |
| W-P5 | wasm SIMD128 なし | ソース内 0 件 (AVX2 のみ、 x86 限定) | スカラ動作 (性能のみ、 P3) |
| W-P6 | **アセット I/O が同期 file I/O** | ifstream/fopen が 19 ファイル (font/FBX/profile/visus) | wasm は MEMFS preload か fetch 化が必要。 demo は shaders しか preload していない |
| W-P7 | WebGL renderer の機能が最小 | icosphere 1 種、 BatchBuilder / text / post 未接続 | 「Web でも Pictor」 と言える水準ではない (リファレンスとしては正直) |
| W-P8 | UBO 実装が未使用 | `webgl_buffer.cpp:90-115` を renderer が使っていない | 死蔵コード (拡張時の足場ではある) |
| W-P9 | level editor が本体と未接続 | 純 JS 再実装 | 二重実装の保守コスト |
| W-P10 | Web 系 CI なし | Emscripten ジョブ不在 | (a) も無検証で腐り得る |

## 3. 設計 (対応方針)

### 決定 1 — 「C++ / Emscripten を正」 に一本化する (W-P1)

実装が存在し spec (`subsystem/webgl.md`) とも一致している (a) を正とする。
`docs/design/webgl_backend.md` (Rust 案) は冒頭に
「**歴史的設計案 — 現行実装は C++/Emscripten (spec/feature/subsystem/webgl.md)**」
の注記を付けてアーカイブ扱いにする (削除はしない: WebGPU 移行パス /
SIMD128 の考察は将来参照価値がある)。

### 決定 2 — Web 対応は 2 段階に分ける (W-P2, W-P3, W-P7)

**Stage 1 (RHI 不要・先行可能): 「Vulkan 非依存コア」 の切り出し**

本体から Vulkan を含まないサブセットをビルドターゲット化する:

```
pictor_core (新 target 案):
  core/types, memory/(frame|pool)_allocator, scene/*, batch/*,
  culling/(frustum|flat_bvh|world_partition), update/* (dispatcher 差替え可),
  text/(font_loader|text_rasterizer), pipeline/pipeline_profile(+serializer)
除外: surface/*, gpu/*, gi/*, pipeline/(compiler|scheduler|recorder|driver),
      profiler/gpu_timer, postprocess/*
```

- 前提整備: `pictor.h` から Vulkan 系 include を外し `pictor_vulkan.h`
  opt-in umbrella に分離する (= review M-4 の是正と同一作業。 移植とレビュー
  指摘が同じ修正に合流する)。
- これで wasm 側は 「SoA scene + cull + batch + text ラスタライズ」 を
  そのまま使い、 描画だけ `pictor_webgl` (WebGL2) が担う構成にできる。
  `webgl_renderer` が BatchBuilder の `RenderBatch` を受ける口を作るのが
  W-P7 / W-P8 の解消点 (UBO はここで使う)。

**Stage 2 (RHI 導入後): 描画経路の共通化**

`dx12-backend-design.md` の `IRhiDevice` / `IRhiCommandEncoder` を導入した
時点で、 CompiledGraph 実行のバックエンドとして WebGPU 実装を追加する。
WebGL2 は compute / indirect が無いため **RHI の対象にしない**
(Stage 1 の並置リファレンスのまま維持し、 Web の本命は WebGPU とする)。

### 決定 3 — スレッド / アセット方針 (W-P4, W-P6)

- 既定は **シングルスレッド wasm** (COOP/COEP を要求しない)。
  `IJobDispatcher` に `SingleThreadDispatcher` を実装し、 UpdateScheduler の
  strategy 選択が thread 数 1 で自然に直列化する現挙動を利用する。
  `-pthread` ビルドは opt-in (ホスティング側がヘッダを設定できる場合のみ)。
- アセットは Emscripten `--preload-file` (MEMFS) を第一段とし、
  ifstream 系ローダは無改修で通す。 fetch / 非同期化は
  `IResourceLoader` (visus に既存) の wasm 実装として後置。

### 決定 4 — CI (W-P10)

emsdk の **コンパイルのみ** ジョブを追加 (`pictor_webgl` + demo、 Stage 1
以降は `pictor_core` も)。 mobile.md Phase A と同じ思想:
実行はしない、 腐敗だけ止める。

### 非対応と明示するもの

- WebGPU 実装 (Stage 2 まで着手しない)
- WebGL2 での GPU-driven / compute / post-process (API 制約 —
  `subsystem/webgl.md` の非対応リストを維持)
- level editor の本体接続は Stage 1 の `pictor_core` 完成後に
  wasm ビルドへ置換する (それまで JS 実装を凍結維持)
