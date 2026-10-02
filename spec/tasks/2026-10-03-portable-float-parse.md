---
task: portable-float-parse
project: Pictor
kind: 実装
created: 2026-10-03
actio: actio:4d1c34c0-c904-4828-9567-995a80ac3de9
source_session: lictor-3400b83d-a27d-40bd-98ff-dc037c93d09f
memory_links:
  - spec/feature/android-build-validation.md
  - spec/feature/portability.md
---
# 浮動小数点 std::from_chars をポータブルな解析へ置き換える

## 目的

浮動小数点の `std::from_chars` は Apple libc++ (macOS / iOS SDK) と Android NDK r27
までの libc++ で提供されず、Pictor 本体がそれらの環境でコンパイルできない。
上流で locale 非依存のポータブルな解析に置き換え、利用側の互換 shim を不要にする。

## 洗い出し結果

| 箇所 | 種別 | 対応 |
|---|---|---|
| `src/visus/visus_json.cpp` `Parser::parse_number` | double の `from_chars` (+ Android 限定 stream 変換) | `float_parse::parse_double` へ置換 |

整数の `from_chars` はリポ内に無く、浮動小数点の呼び出しは上記 1 箇所のみ。
エンコード側 (`to_chars` / Android stream 出力) は本タスクの対象外。

## 設計

- `include/pictor/core/float_parse.h` / `src/core/float_parse.cpp` (責務: 浮動小数点解析のみ)。
- `[first, last)` を from_chars `general` 文法で先に検証し、一致した範囲だけを NUL 終端
  バッファへ写して C ロケール固定の strtod 系で変換する。
  - Windows: `_strtod_l` + `_create_locale(LC_NUMERIC, "C")`
  - Apple / glibc: `strtod_l` + `newlocale`
  - その他 POSIX (bionic 等): `uselocale` + `strtod`
- ERANGE は無限大 (overflow) と 0 (underflow) の場合だけ `result_out_of_range`。
  subnormal は受理する (from_chars と同じ)。エラー時は値を書き換えない。

## 完了条件

- C-1 parse_double(first, last, value): 正常値・指数表記・負数を from_chars と同値に解析し、読み終え位置 `ptr` を返す
- C-2 parse_double(first, last, value): 末尾の余分な文字 / 不完全な指数を読まず、`[first, last)` の外を読まない
- C-3 parse_double(first, last, value): 空文字列・先頭空白・`+`・`,` 始まりは `invalid_argument`、値は不変
- C-4 parse_double(first, last, value): `"1,5"` は `,` で止まり、`,` 小数点ロケール下でも結果が変わらない
- C-5 parse_double(first, last, value): overflow と非ゼロ値の underflow-to-zero は `result_out_of_range`、subnormal は受理
- C-6 Parser::parse_number(out): Visus JSON の数値受理 / 拒否 (overflow・underflow を "number out of range") が置換前と同じ

## 再利用探索

- 既存の Android 限定 stream 変換 (`std::istringstream` + `std::locale::classic()`): 不採用。
  `"1,5"` の扱いや underflow 判定を数字列の再走査で補っており、読み終え位置を返せず
  from_chars の契約 (ptr / errc の区別) を表現できない。全プラットフォーム共通化の土台にしない。
- 外部ライブラリ (fast_float 等) の同梱: 不採用。依存追加 (third_party) を伴い、本件は
  C ランタイムの locale 固定 strtod で要件を満たせる。
- `pictor/core/parse_limits.h` と同じく、内部パーサが共有する小さな公開ユーティリティとして
  `include/pictor/core/` に置く配置規約を再利用した。

## 変更した境界

- 新規公開ヘッダ `pictor/core/float_parse.h` (`pictor::float_parse::parse_double`)。
- `visus_json.cpp` の数値解析: 全プラットフォームで `parse_double` を使う。
  受理する文字列・エラー文言 ("number out of range") は変更なし。
- エンコード側 (`to_chars` / Android 限定 stream 出力) と整数解析は変更なし。

## 検証

- 実施: Windows / MSVC (VS 2022 14.39) Debug で `pictor` と全テストをビルド、ctest 実行。
  `unit_float_parse_test` は Pass。
- 実施: ctest の既存失敗 3 件 (`unit_visus_resource_loader_test` / `unit_compiled_graph_wiring_test` /
  `fps_baseline_test`) は、変更前の HEAD (02ea861) を別ビルドしても同じく失敗することを確認
  (本変更と無関係)。
- 未実施: GCC (Ubuntu) のローカルビルド。手元の WSL が起動できなかったため CI に委ねる。
- 未実施: macOS (Apple clang) / Android NDK r27 での実ビルド (環境なし)。
