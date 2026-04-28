# Pictor Feature Selector

`libpictor` に入れたい機能を **ブラウザ上で対話的に選んで CMake コマンドを
生成する** ためのツールです。推定サイズがリアルタイムに更新されるので、
ビルド前にサイズ感を掴めます。

## 使い方

### パターン A — ファイルを直接開く (推奨)

```bash
# Windows
start tools\feature-selector\index.html

# macOS
open tools/feature-selector/index.html

# Linux
xdg-open tools/feature-selector/index.html
```

`file://` で開いて OK。バックエンド不要、純粋に静的 HTML + vanilla JS です。

### パターン B — CMake custom target から

```bash
cmake -S . -B build -DPICTOR_BUILD_TOOLS=ON
cmake --build build --target pictor_feature_selector   # ブラウザを起動
```

### パターン C — 静的サーバ経由 (file:// が不安なら)

```bash
cd tools/feature-selector
python3 -m http.server 8765
# http://localhost:8765/
```

## 画面

| 場所 | 内容 |
|---|---|
| 左ペイン | カテゴリ別のフィーチャ一覧。クリックで toggle。各項目に推定サイズ・影響ソース・外部依存が並ぶ |
| 右ペイン (Presets) | "Shipping", "Shipping + Rive", "Development (default)", "Web (WebGL2)", "Kitchen sink" — ワンクリック適用 |
| 右ペイン (Size bar) | 現在の構成で推定される `libpictor` サイズ。base + 有効機能のデルタ合算 |
| 右ペイン (CMake command) | 生成された `cmake -S . -B build -D...` 一行。Copy ボタンでクリップボードへ |
| 右ペイン (State URL) | 現在の設定は URL hash に入っているので、そのまま共有可能 |

## 推定サイズを **実測値** に更新する

推定サイズは authored (`features.js`) + ソース行数ベースの概算です。
正確な数値が必要なら:

```bash
cmake -S . -B build -DPICTOR_BUILD_TOOLS=ON
cmake --build build --target pictor_measure_sizes
```

あるいは直接:

```bash
python3 tools/feature-selector/scripts/measure_sizes.py \
    --src-dir . \
    --build-dir /tmp/pictor-measure \
    --config Release \
    --out tools/feature-selector/measurements.json
```

ターゲットは全 feature を OFF にした **baseline** と、**1 機能だけ ON** の
6 種類を順にビルドして `libpictor.(a|lib)` サイズを測り、JSON に差分を
書き出します。現状 UI は `measurements.json` を自動で読み込みませんが、
手動マージは `features.js` の `size_delta_kb` に書き戻す形で行います
(将来の改善ポイント)。

## 追加するフィーチャがあるとき

1. `CMakeLists.txt` に `option(PICTOR_NEW_OPTION ...)` を足す
2. `tools/feature-selector/features.js` の該当カテゴリに 1 行追加
3. 場合によっては `PRESETS` セクションを見直す
4. `scripts/measure_sizes.py` の `ALL_FEATURES` に追加

設計上、UI はサーバレス前提なので、JSON ではなく ES モジュール
(`features.js`) でマニフェストを記述している点に注意してください
(ブラウザが `file://` で fetch できないため)。

## なぜ「GUI で選ぶ」のか

Pictor は Rive (+4.8MB) / WebGL (+360KB) / Profiler (+230KB) など
依存が重いオプションが複数あります。shipping 時に本当に必要な機能だけ
選べば `libpictor` を 3MB 前後まで絞り込めます。CLI だけだと
「何をどう組み合わせるか」が見えづらいので、ワンページで全体像を
俯瞰できるツールを用意しました。
