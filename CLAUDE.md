# Pictor — Claude Code ルール

## プロジェクト概要

Pictor は **Data-Driven Rendering Pipeline Module** (C++20、 AVX2、 Vulkan)。
ObjectDescriptor 入力で render path を自動選択するシステムレイヤのレンダリング基盤。
LUDIARS 短縮コード: **Pc**。

## コード規約 (Pictor 固有)

共通: `coding-conventions` skill (= `AIFormat/RULE_CODE.md`) を参照。 以下は Pictor 固有の上書き / 追加。

### データ配置 (DoD 重視)

- **per-frame hot path**: flat array + cache-line aligned (`alignas(64)`)。 struct-of-arrays を基本。
- **scatter 禁止**: hot path に `std::map<>` / 個別 `new` / pointer chase を増やさない。 instance データは flat container に詰める ([[feedback_pictor_dod_layout]])。
- **参照データ**: init で 1 回ロード、 frame 内でコピー / 再 alloc しない。
- **alignment**: SIMD 前提構造体は `alignas(16)` 以上、 cache line 跨ぎは `alignas(64)`。 メモリ配置に意図がある場合はコメント必須。

### OOP (公開 API)

- 公開 API は class + virtual interface で表現 (= ハンドル / Material / RenderPath 等)。
- 内部実装は DoD だが、 **境界 API は OOP** で「データ表現の自由度」 を保つ (= consumer に内部 layout を漏らさない)。
- 値型 (`struct`) と参照型 (`class`) は意図的に使い分ける。

### SOLID 全 5

- **S (Single Responsibility)**: 共通通り。 描画 path / Material / Buffer / Memory pool は別ファイル。
- **O (Open/Closed)**: 新しい描画 path / Material kind は **継承 / Registry** で増やす。 既存 path の switch 文に case を増やすのではなく、 新 class を register する。
- **L (Liskov Substitution)**: RenderPath / Material 派生は親契約を守る (= `submit()` の呼出順 / 例外 / GPU state を変えない)。
- **I (Interface Segregation)**: 「太い IRenderer 1 個」 ではなく `ICuller` / `IShader` / `IPass` 等に分離。
- **D (Dependency Inversion)**: consumer は Pictor の **抽象 interface に依存**、 Vulkan handle / GLFW を直叩きさせない (= Pictor 内部で wrap)。

### レイヤ依存

- **Pictor は最下層**。 上位ライブラリ (Ergo / AdventureCube / KuzuSurvivors / ergo_custos 等) を Pictor demo / test に取り込まない ([[feedback_pictor_no_upper_dep]])。
- Pictor → Vulkan / GLFW / 標準ライブラリのみ。

### ビルド / 実行 / テスト

- **MSVC `/utf-8`**: `target_compile_options(... PRIVATE /utf-8)` は consumer に伝播しないので、 test target に個別指定 ([[feedback_msvc_utf8_test_targets]])。
- **Debug 既定**: 診断ログは Debug でしか出ない。 普段の実行確認も Debug build を使う ([[feedback_pictor_debug_default]])。
- **実行は禁止**: ビルドまで。 `.exe` 起動・確認はユーザー側 ([[feedback_pictor_no_run]])。 セッションごとに override 可。

## 参照

- `coding-conventions` skill / [[project_pictor_visus]] / [[project_pictor_rive]] / [[feedback_pictor_dod_layout]] / [[feedback_pictor_no_upper_dep]] / [[feedback_pictor_debug_default]] / [[feedback_pictor_no_run]] / [[feedback_msvc_utf8_test_targets]]
