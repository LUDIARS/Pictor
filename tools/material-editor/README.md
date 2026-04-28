# Pictor Material Editor

Pictor の `MaterialDesc` をブラウザで編集し、**C++ 側の
`to_material_json()` と完全互換な JSON** に書き出すためのツール。
`file://` で開けばそのまま動きます — バックエンド不要、vanilla
ES modules のみ。

## 使い方

### パターン A — ファイルを直接開く

```bash
# Windows
start tools\material-editor\index.html

# macOS
open tools/material-editor/index.html

# Linux
xdg-open tools/material-editor/index.html
```

### パターン B — 静的サーバ経由 (一部環境で module resolver が file:// を嫌う場合)

```bash
cd tools/material-editor
python3 -m http.server 8766
# http://localhost:8766/
```

## UI 構成

| 場所 | 内容 |
|---|---|
| Identity   | マテリアル名 (任意、JSON 側 `name` にそのまま入る) |
| Textures   | 6 スロット (albedo / normal / metallic / roughness / ao / emissive)。値は `"handle:<N>"` もしくは `"none"` の文字列 |
| Base       | base_color (RGBA) と emissive (RGB)。ピッカー + 個別数値欄の双方向バインド |
| Scalars    | metallic / roughness / alpha_cutoff / normal_scale / ao_strength のスライダ + 数値入力 |
| Flags      | cast_shadow / receive_shadow のトグル。`features` ビットは **他フィールドから自動計算** (C++ 側 `BaseMaterialBuilder::compute_features()` と同等) |
| JSON       | 現在の状態のライブプレビュー。New / Import / Export / Copy のボタン |

状態は URL hash に base64 で入るため、URL をそのまま共有すれば
同じマテリアルが復元されます。

## JSON レイアウト

C++ `pictor::to_material_json()` と同じ canonical 形式で出力します。
詳細は `include/pictor/material/material_serializer.h` を参照。

```json
{
  "version": 1,
  "name": "matte_red",
  "textures": {
    "albedo":    "handle:42",
    "normal":    "none",
    "metallic":  "none",
    "roughness": "none",
    "ao":        "none",
    "emissive":  "none"
  },
  "params": {
    "base_color":   [0.8, 0.1, 0.1, 1.0],
    "emissive":     [0, 0, 0],
    "metallic":     0,
    "roughness":    0.75,
    "alpha_cutoff": 0,
    "normal_scale": 1,
    "ao_strength":  1
  },
  "flags": {
    "cast_shadow":    true,
    "receive_shadow": true,
    "features":       1025
  }
}
```

## Ars との連携 (将来)

現状は **JSON ファイルを単一の接点** として運用する想定:

1. このエディタで JSON を保存 (project 側の `materials/*.json`)
2. Ars 側のアセット import で JSON を読み、`from_material_json()`
   経由で `MaterialDesc` を構築、`BaseMaterialBuilder` へ流し込む

リアルタイム連携 (WS でホットリロード等) は Ars plugin 側での
実装対象。
