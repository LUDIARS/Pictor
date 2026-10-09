# フレームタップ (Frame Tap)

> 対象: `include/pictor/tap/` / `src/tap/`。所属ドメイン: `profiling` (開発支援)。
> 描画リストの形の正本は Commentarii の `spec/feature/render-tap-contract.md` と
> `schema/render-frame.schema.json` (Commentarii 段階 6)。本書は Pictor 側の出し方を定める。

## 1. 目的と重視点

Pictor がフレームごとに描いた物 (どのメッシュ / マテリアルが、どの変換で、画面のどこに、
どの深度順で) を構造化データとして外へ出す。外部ツール (攻略本ツクールの描画タップ受信等) が
画面の内容を画素認識なしに読むための口。

重視点:

1. **既定 OFF・OFF 時は分岐 1 つ** — `PictorRenderer` の `render()` / `end_frame()` に
   `if (frame_tap_.is_enabled())` を 1 つずつ置くだけ。OFF 時は確保も走査もしない。
2. **GPU を待たない** — 画素や GPU バッファを読み戻さない。CPU 側が既に持つ
   SoA ストリーム (変換・境界・可視フラグ・ハンドル) を読むだけで、GPU 同期は増やさない。
3. **ID はアセット名由来** — ハンドル番号はビルドや登録順で変わるため出さない。
4. **依存を足さない** — JSON は手書きの 1 行エンコーダで出す。

## 2. 有効化

| 経路 | 書き方 | 意味 |
|---|---|---|
| 環境変数 | `PICTOR_FRAME_TAP=stdout` (または `-`) | 標準出力へ JSON Lines |
| 環境変数 | `PICTOR_FRAME_TAP=<path>` | `<path>` へ追記 (バイナリ追記、改行は LF) |
| 環境変数 | 未設定 / 空 / `0` / `off` | 無効 (既定) |
| 起動オプション | `RendererConfig::frame_tap` | 環境変数と同じ書式。空でなければ環境変数より優先 |
| API | `renderer.frame_tap().set_sink(std::make_unique<CallbackFrameTapSink>(fn))` | 呼び出し側のコールバックへ 1 行ずつ |
| API | `renderer.frame_tap().disable()` | 無効化 |

- ファイルを開けない場合は stderr に理由を出してタップは無効のまま (無言で別経路へ逃げない)。
- `stdout` を使う場合、ホストは標準出力へ他のログを混ぜないこと (タップの警告は stderr に出す)。
  Windows の標準出力はテキストモードのため改行が CRLF になる。改行を LF に固定したい場合はファイル出力を使う。

## 3. 出力形式 (1 フレーム 1 行)

```json
{"frame":12,"t":0.2,
 "camera":{"view":[16 数],"projection":[16 数],"viewport":[1280,720]},
 "passes":[
  {"pass":"scene","draws":[
   {"mesh":"crate","material":["wood"],"instance":3,"world":[16 数],
    "screen_bbox":[412.5,300,96,80],"depth_order":0,"tags":[]}]},
  {"pass":"ui","draws":[
   {"mesh":"ui:rect","material":[],"instance":0,"world":[16 数],
    "screen_bbox":[16,16,200,24],"depth_order":0,"tags":[]}]}]}
```

(実際の出力は改行を含まない 1 行。)

| フィールド | 内容 |
|---|---|
| `frame` | `PictorRenderer` のフレーム番号 (`begin_frame` ごとに 1 増える) |
| `t` | タップ有効化後の経過秒。ホストが `begin_frame` に渡した `delta_time` の累積 (時計を読まない) |
| `camera.view` / `camera.projection` | 4x4 行列。`m[row][col]` を行優先で 16 個 (Pictor の行ベクトル規約、平行移動は 12〜14 番目) |
| `camera.viewport` | `screen_bbox` の基準となる画面サイズ [幅, 高さ] (px) |
| `passes[]` | `scene` と `ui` を必ずこの順で 1 つずつ。ポストプロセス pass は含めない |
| `mesh` | メッシュ ID (§4) |
| `material[]` | マテリアル ID の配列 (§4)。名前の取れないマテリアルは入れない |
| `instance` | scene: `ObjectId`。ui: その UI 描画リスト内の添字 |
| `world` | 4x4 行列 (camera と同じ並び)。ui は単位矩形を画面 px の矩形へ写す行列 |
| `screen_bbox` | [x, y, w, h] (px、左上原点、y 下向き)。scene は world AABB の 8 隅を投影した外接矩形 (§5) |
| `depth_order` | pass 内の前後順。0 が最前面。scene は視点からの深度、ui は後に描いたものが前 |
| `tags[]` | `unnamed` (アセット名が取れなかった) / `offscreen` (投影結果が画面外) |

数値が有限でない場合は `null` を出す (0 などの偽値に置き換えない)。

## 4. ID の決め方

- **メッシュ**: `register_mesh_data()` の `MeshDataDescriptor::name`、または
  `FrameTapNameTable::set_mesh_name()` で与えた名前。名前の取れないメッシュは
  指紋 `fp:<16 桁 hex>` (頂点数 + インデックス数の FNV-1a 64bit) で代用し、`tags` に `unnamed` を付ける。
- **マテリアル**: Pictor の実行時マテリアルは名前を持たないため、ホストが
  `FrameTapNameTable::set_material_name()` で与える。与えていなければ `material[]` に入れず
  `tags` に `unnamed` を付ける (ハンドル番号は出さない)。
- **UI**: メッシュは描画種別 (`ui:rect` / `ui:nine_slice` / `ui:image`)、マテリアルは
  `FrameTapNameTable::set_ui_texture_name()` で与えたテクスチャ名。純色矩形 (texture 0) は空配列。

## 5. screen_bbox の求め方

world AABB の 8 隅を `view * projection` でクリップ空間へ送り、視点の前 (w > ε) にある隅と、
前後をまたぐ 12 辺と w = ε 面の交点を射影して外接矩形を取る。頂点は走査しない。
結果は viewport で切り詰め、面積が無ければ `[0,0,0,0]` と `offscreen` タグ。

## 6. 何を記録するか

- scene: static / dynamic プールのうちカリングで可視になった物 (BatchBuilder がバッチにする集合と同じ)。
  GPU-driven プールは GPU 側でカリングするため CPU に可視集合が無く、読み戻しをしない原則により対象外。
- ui: `UIRenderer::set_frame_tap()` で渡したタップへ、`record()` が Rect / NineSlice / Image を記録する。
  Text (UIRenderer が描かない) と Push/PopClip は描画ではないので出さない。
  `screen_bbox` は描画コマンドの矩形そのもので、PushClip による切り抜きは反映しない。
  テクスチャ ID は UIRenderer が実際に束縛した値 (範囲外は 0 = 純色) を使う。
- `render()` より前、または `end_frame()` より後に届いた描画は捨て、`dropped_draws()` に数える (初回のみ stderr に警告)。

## 7. 構成 (1 ファイル 1 責務)

| ファイル | 責務 |
|---|---|
| `frame_tap_types.h/.cpp` | 1 フレーム分の記録の値型と pass 名 |
| `frame_tap_sink.h/.cpp` | 1 行の配送先 (stdout / ファイル追記 / コールバック) |
| `frame_tap_config.h/.cpp` | `PICTOR_FRAME_TAP` 書式の解釈と配送先の生成 |
| `frame_tap_asset_source.h` | アセット名の問い合わせ口 (interface) |
| `frame_tap_name_table.h/.cpp` | ホストが与える名前表 (interface の既定実装) |
| `frame_tap_asset_ids.h/.cpp` | 名前 / 指紋からの安定 ID |
| `frame_tap_projection.h/.cpp` | AABB の画面投影と視点深度 |
| `frame_tap_json_line.h/.cpp` | 1 フレームを 1 行の JSON にする |
| `frame_tap.h/.cpp` | フレームの開閉・描画の記録・深度順の確定・配送 |
| `scene_frame_tap_collector.h/.cpp` | SceneRegistry の可視オブジェクトをタップへ流す |

`PictorRenderer` は `frame_tap()` と `frame_tap_names()` を公開し、`register_mesh_data()` /
`unregister_mesh_data()` のたびにメッシュ名と頂点数・インデックス数を名前表へ写す
(タップが OFF でも登録時に 1 回だけ。フレームごとのコストは無い)。

## 8. 復旧

タップは観測専用で描画結果を変えない。不具合時は `PICTOR_FRAME_TAP` を外す
(または `RendererConfig::frame_tap` を空にする) と、OFF 時の 1 分岐以外は変更前と同じ経路になる。
