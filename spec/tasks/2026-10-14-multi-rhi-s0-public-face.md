# 実装: 描画方式の切り替え口 (RHI) 段階 S0 — 公開面・機械検査・null 方式

- 日付: 2026-10-14
- 対象: Pictor (Pc)
- Actio: `actio:9998f9a7-0358-401e-a5e7-79662fdf7bff` (親: `a38aab5b-3141-4093-a108-a71722bd25b0`)
- 設計の正本: [`spec/architecture/multi-rhi.md`](../architecture/multi-rhi.md) §1 境界、§6 S0 行と機械検査 C1 / C2 / C4
- ブランチ: `feat/rhi-s0-public-face`

## 分解

| # | 内容 | 置き場所 |
|---|---|---|
| 1 | 方式中立の公開面の型 | `include/pictor/gfx/` (`backend_kind` / `device_caps` / `backend_report` / `handle` / `format` / `load_store_op` / `compare_op` / `resource_desc` / `pipeline_desc` / `shader_bundle_ref` / `pass_command` / `pass_recorder` / `pass_context` / `gfx.h`)、名前表は `src/gfx/` |
| 2 | null 方式 (`PICTOR_RHI_NULL`、既定 ON、PRIVATE マクロ) | `src/rhi/null/` (資源表 / パス記録 / device を別ファイル)、ハンドル表の共通部は `src/rhi/common/handle_allocator` |
| 3 | 機械検査 C1 / C2 / C4 と CI 組み込み | `tools/consumer_face/`、`cmake/PictorConsumerFaceChecks.cmake`、`.github/workflows/build.yml` (既存 Ubuntu GCC ジョブ) |
| 4 | `pictor.h` から Vulkan 系 include を外し `pictor/pictor_vulkan.h` へ | demo 9 本と `tests/fps_baseline_test.cpp` を `pictor_vulkan.h` へ付け替え (挙動不変) |
| 5 | `pictor_core` (方式非依存部) の切り出しと Emscripten 構成 | `cmake/PictorCore.cmake`。`SingleThreadDispatcher`、aligned alloc を `src/memory/aligned_block` に集約 |
| 6 | `PICTOR_HAS_RHI` 追加 | `pictor_core` の PUBLIC 定義 (`pictor` 経由で伝播)。`PICTOR_HAS_VULKAN` 等は S4 まで併存 |
| 7 | 単体テスト | `tests/unit_gfx_handle_test` / `unit_pass_recorder_test` / `unit_null_backend_test` / `unit_single_thread_dispatcher_test` (いずれも `pictor_core` のみリンク) |

## 受け入れ条件 (契約書式)

- C-1 `include/pictor/gfx/*.h`: 方式の型・名前・マクロ・include を含まない。C1 検査で該当 0 件、かつ方式マクロ未定義で各ヘッダが単独コンパイルできる (C2 後半)
- C-2 `check_public_headers.py`: `include/pictor/**` (interop を除く) を走査し、allowlist 外の該当で失敗、allowlist の掃除漏れで失敗する。現状の allowlist 件数を確定する
- C-3 `check_macro_propagation.py`: `pictor_core` は方式マクロを 1 つも出さない。`pictor` は S4 まで残す既存マクロ以外を出さない。両方 `PICTOR_HAS_RHI` を出す
- C-4 `report_interop_usage.py`: `demo/` `tools/` `tests/` の interop include 数と方式識別子を含むファイル数を報告し、基準値からの増加で失敗する
- C-5 `PassRecorder`: 呼び出し順のまま 32 B の POD `PassCommand` を flat 配列へ追記し、仮想呼び出しを持たない。不正ハンドル・group 範囲外・compute の無い device での Dispatch は追記せず数える
- C-6 `HandleAllocator` / `gfx::Handle`: 32bit (index 20 / generation 12)。解放で世代が進み、古いハンドルは解決しない。世代 0 は発行しない
- C-7 `NullDevice`: 資源のハンドルと記述子を保持し、フレーム内に submit されたパスのコマンドを記録するだけで描かない。記録エラーのあるパスは拒否、死んだ資源の参照は数える
- C-8 `pictor.h`: 推移的に GPU API ヘッダへ到達しない。従来の利用箇所は `pictor_vulkan.h` で同じ API を得る
- C-9 `pictor_core`: Vulkan / GLFW に依存せず、Emscripten toolchain でも構成される (pthreads 無しは `SingleThreadDispatcher` が既定)

## 決定と理由

- C1 の allowlist は **41 本** (設計書の見込みは 42 本)。設計書の数字は `Vk*` の概数で、本実装はコメント除去・トークン単位で数えた。
  コメントにだけ方式名が出るヘッダ (`core/types.h`、`surface/surface_provider.h` など 14 本) は該当にしない。
- C1 の規則には方式マクロ (`PICTOR_HAS_VULKAN` / `PICTOR_RHI_*` 等) も含める。公開ヘッダが方式マクロで分岐することも「消費側が方式を知る」に当たるため。
- `pictor_vulkan.h` は移行期間の interop 扱い (C4 で利用数を数える)。`include/pictor/backend/*_interop.h` はまだ無い。
- `RecordError` の正常値は `Ok`、`CompareOp` の定数比較は `NeverPass` / `AlwaysPass`。X11 の `None` / `Always` マクロとの衝突を避ける。
- `DeviceCaps` の Feature は 64 bit の `FeatureSet` (std::bitset ではなく trivially copyable)。
- `PassContext` に方式名・能力を載せない (独自パスが方式で分岐できないようにする)。
- `SingleThreadDispatcher` は chunk 0 を「1 ジョブ」として扱う (既存 `ThreadPoolDispatcher` は 0 除算になる)。

## 外部への影響

- `pictor/pictor.h` から `PictorRenderer` / `VulkanContext` / GPU timer / compiled graph 記録 / host 駆動 post-process が外れた。
  これらを使う外部消費側 (確認できたもの: AdventureCube `src/main.cpp`) は `pictor/pictor_vulkan.h` へ付け替える必要がある。
- `pictor` は `pictor_core` を PUBLIC リンクする。`libpictor` 単体のサイズは小さくなる (feature-selector の測定値は次回測定で変わる)。

## 検証

- 実施: MSVC 19.39 Debug で全ターゲット (pictor・全 demo・全 tests・`pictor_gfx_header_check`) のビルド成功。新規ファイルの警告 0。
  C1 / C2 (MSVC 構成) / C4 のスクリプト実行。
- 未実施: テストの実行 (Revisor 審査に委ねる)、`.exe` の起動 (Pictor の規約)、GCC ビルド (手元に無い。CI の Ubuntu GCC ジョブで確認)、
  Emscripten での `pictor_core` コンパイル (emsdk が手元に無い。構成のみ用意し未確認)。
