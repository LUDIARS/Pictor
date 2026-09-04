# Anatomia のプログラムドメイン層宣言 (`.anatomia/layers.json`)

Anatomia のドメイン判定は 2 軸ある。

| 軸 | 何を表すか | 正本 |
|---|---|---|
| ビジネスドメイン | 「何の機能か」 | `spec/domains/*.domain.json` |
| **プログラムドメイン (層)** | **「どの層か」** | **`.anatomia/layers.json`** |

このファイルは後者の正本。**リポジトリが持つ設定であって解析キャッシュではない**ので、
`.gitignore` で `.anatomia/*` + `!.anatomia/layers.json` として明示的に追跡している。

## なぜ要るか

`layers.json` が無いと Anatomia は層を 1 つも決められず、変更された anchor が**全部**
「未分類 (`unclassified`)」として報告される。Revisor のレビューではこれが
`Anatomia dual-layer (program): N changed anchor(s) unclassified` という非ブロック所見になる。

PR ごとに件数が違うのは変更量の差でしかなく、原因は個々の PR ではない。

## 書き方の注意 (踏むと分類が壊れる)

1. **glob はファイルパスに完全一致で当たる。** `*` は 1 階層、`**` は任意階層。
   前方一致ではないので `src/routes` ではなく `src/routes/*` と書く。
2. **記述順は効かない。** ローダーが glob 文字列でソートしてから先勝ちで評価する
   (`Anatomia/src/domains/program/config.ts`)。読みやすさのために層ごとに並べてよいが、
   優先順位を順序で表現してはいけない。
3. **したがって glob 同士を重ねない。** モジュール 1 つにつき `<dir>/*` を 1 本置き、
   入れ子のディレクトリには別途 1 本置く。`src/**` のような広い glob を 1 本置くと、
   ソート順しだいで `src/routes/*` を飲み込んで層が入れ替わる。
4. **`.gitignore` でディレクトリごと除外しない。** `.anatomia/` の形だと git は中のファイルを
   `!` で再包含できない。`.anatomia/*` + `!.anatomia/layers.json` の形にする。
5. **未追跡かつ ignore もされていないファイルは解析対象に入る** (`Anatomia/src/fs/git-ignore.ts`)。
   ビルド成果物を `.gitignore` に入れ忘れると、ソースのコピーが丸ごと二重に解析される。

## 確認のしかた

```sh
node ../Anatomia/bin/anatomia.mjs domains program --repo .
```

上記は Anatomia と本リポジトリを同じ親ディレクトリに checkout した場合の例。別の配置では
`../Anatomia` を Anatomia checkout への相対パスに置き換える。

`unclassified: 0 module(s), 0 symbol(s)` になっていれば二層ゲートは通る。
新しいディレクトリに実装を足したらこのコマンドを回して 0 を保つこと。
ゲートではなくレンズなので、放置しても exit code は 0 のまま静かに増える。
