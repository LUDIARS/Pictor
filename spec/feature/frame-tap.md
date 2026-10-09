# フレームタップ (Frame Tap)

> 対象: `include/pictor/tap/` / `src/tap/`。所属ドメイン: `profiling` (開発支援)。
> 描画リストの形の正本は Commentarii の描画タップ契約 **render-tap/1**
> (`spec/feature/render-tap-contract.md`、`schema/render-frame.schema.json`、
> golden `tests/fixtures/render-tap/golden-v1.jsonl`)。本書は Pictor 側の出し方を定める。
> 契約が版を上げたら本書と `kFrameTapContract` を合わせて上げる。

## 1. 目的と重視点

Pictor がフレームごとに描画へ出した物 (どのメッシュ / マテリアルが、どの変換で、画面のどこに、
どの順で、どんな可視性の根拠で) を構造化データとして外へ出す。外部ツール (攻略本ツクールの
描画タップ受信等) が画面の内容を画素認識なしに読むための口。

**タップの行は「描画に出したもの (raw tap)」であり「プレイヤーが見たもの」ではない。**
見えたかどうかの判定 (pass の種類・遮蔽の根拠・alpha・clip) は受信側が契約に従って行う。
Pictor は判定に要る根拠を、GPU を待たずに持っている範囲で正直に出す。

重視点:

1. **既定 OFF・OFF 時は分岐 1 つ** — `PictorRenderer` の `render()` / `end_frame()` に
   `if (frame_tap_.is_enabled())` を 1 つずつ置くだけ。OFF 時は確保も走査もしない。
2. **GPU を待たない** — 画素や GPU バッファ・クエリ結果を読み戻さない。CPU 側が既に持つ
   SoA ストリーム (変換・境界・可視フラグ・ハンドル) を読むだけで、GPU 同期は増やさない。
3. **ID はアセット名由来** — ハンドル番号はビルドや登録順で変わるため mesh / material に出さない。
4. **依存を足さない** — JSON は手書きの 1 行エンコーダで出す。
5. **契約を破る行を出さない** — 数値は有限、t / tick は減らない、instance の外見が変われば世代を上げる。
   守れない draw / フレームは出さずに数える (§7)。

## 2. 有効化

| 経路 | 書き方 | 意味 |
|---|---|---|
| 環境変数 | `PICTOR_FRAME_TAP=stdout` (または `-`) | 標準出力へ JSON Lines |
| 環境変数 | `PICTOR_FRAME_TAP=<path>` | `<path>` へ追記 (バイナリ追記、改行は LF) |
| 環境変数 | 未設定 / 空 / `0` / `off` | 無効 (既定) |
| 起動オプション | `RendererConfig::frame_tap` | 環境変数と同じ書式。空でなければ環境変数より優先 |
| API | `renderer.frame_tap().set_sink(std::make_unique<CallbackFrameTapSink>(fn))` | 呼び出し側のコールバックへ 1 行ずつ |
| API | `renderer.frame_tap().disable()` | end 行 (`shutdown`) を出して無効化 |
| API | `renderer.frame_tap().end_stream(FrameTapEndReason::FAILED)` | end 行 (`error`) を出して無効化 |

- ファイルを開けない場合は stderr に理由を出してタップは無効のまま (無言で別経路へ逃げない)。
- `stdout` を使う場合、ホストは標準出力へ他のログを混ぜないこと (タップの警告は stderr に出す)。
  Windows の標準出力はテキストモードのため改行が CRLF になる。改行を LF に固定したい場合はファイル出力を使う。

### 2.1 ホストが与える値

| API | 行の項目 | 与えないとき |
|---|---|---|
| `frame_tap().set_observer_id("player-camera")` | `observer.id` | `player-camera` (書式 `^[a-z0-9][a-z0-9_.-]*$`、合わなければ false で変えない) |
| `frame_tap().clock().set_game_time(seconds)` | `t` | `begin_frame()` の `delta_time` の累積 (タップ有効化からの秒) |
| `frame_tap().clock().set_game_tick(tick)` | `tick` | 出さない (任意項目) |
| `frame_tap().set_visibility_source(&src)` | scene の `visibility` / `visibility_lag_frames` | `frustum-only` / 出さない (§6) |
| `frame_tap().add_pass(name, kind)` + `FrameTapSceneObject::pass` | 追加の pass (影・反射など) | scene / ui の 2 pass だけ |
| `UIRenderer::set_frame_tap_annotations(items, count)` | ui の `ui` / 要素 instance | 意味なし・描画リストの添字 |
| `frame_tap_names().set_material_name()` / `set_ui_texture_name()` | `material[]` | 空 + `unnamed` (§4) |

ゲーム時刻・tick は次に set / `clear_game_clock()` するまで毎フレーム使われる (毎フレーム与えるのが基本)。

## 3. 出力形式 (1 行 1 オブジェクト、UTF-8、LF)

### 3.1 frame 行

```json
{"contract":"render-tap/1","seq":0,"frame":12,"tick":40,"t":4.0,
 "observer":{"id":"player-camera","viewport":[1280,720]},
 "camera":{"view":[16 数],"projection":[16 数]},
 "visibility_lag_frames":1,
 "passes":[
  {"name":"scene","kind":"scene","draws":[
   {"mesh":"crate","material":["wood"],"identity":"asset-name","instance":3,"generation":0,
    "world":[16 数],"screen_bbox":[412.5,300,96,80],"depth_order":0,
    "visibility":"frustum-only","tags":[]}]},
  {"name":"ui","kind":"ui","draws":[
   {"mesh":"ui:rect","material":[],"identity":"asset-name","instance":4294967296,"generation":0,
    "world":[16 数],"screen_bbox":[16,16,200,24],"depth_order":0,"visibility":"unknown",
    "alpha":1,"clip":[0,0,640,360],"ui":{"role":"bar","element":"hud.hp","fill":0.8},"tags":[]}]},
  {"name":"shadow-cascade","kind":"shadow","draws":[...]}],
 "dropped":{"draws":2,"reason":"other"}}
```

(実際の出力は改行を含まない 1 行。`tick` / `visibility_lag_frames` / `dropped` / `alpha` / `clip` / `ui` は任意。)

| フィールド | 内容 |
|---|---|
| `contract` | `"render-tap/1"` (全行) |
| `seq` | タップの出力番号。タップがフレームを開くたびに +1 (出さずに捨てたフレームも数える)。end 行も 1 つ使う。配送先を差し替えると新しい流れとして 0 から |
| `frame` | `PictorRenderer` のフレーム番号 (`begin_frame` ごとに 1 増える、狭義単調増加) |
| `tick` | ホストが与えたゲーム tick。与えたときだけ出る。前の行より小さい値は前の値に留める |
| `t` | ゲーム内時刻 (秒)。ホストが与えた値、無ければ `delta_time` の累積 (§2.1)。有限でない・負・前の行より小さい値は前の値に留める (減らない) |
| `observer.id` | 観測者 (誰の目か)。プレイヤーのカメラは `player-camera` |
| `observer.viewport` | `screen_bbox` / `clip` の基準となる画面サイズ [幅, 高さ] (px、1 以上) |
| `camera.view` / `camera.projection` | 4x4 行列 16 個 (§3.3) |
| `visibility_lag_frames` | 遮蔽の根拠が何フレーム前の結果か。可視性の口 (§6) があるときだけ出る |
| `passes[]` | `{name, kind, draws}`。毎フレーム `scene` (kind scene) → `ui` (kind ui) を必ず開き、ホストが `add_pass()` で足した pass がその後に続く |
| `passes[].kind` | `scene` / `ui` / `shadow` / `reflection` / `depth-prepass` / `postprocess` / `other`。観測者の画面に出うるのは scene / ui だけ |
| `dropped` | そのフレームで記録しきれなかった draw の数と理由 (§7)。無ければ出さない |

### 3.2 draw

| フィールド | 内容 |
|---|---|
| `mesh` / `material[]` | §4 の ID。名前の取れないマテリアルは入れない |
| `identity` | `asset-name` / `content-hash` / `count-hash` (§4) |
| `instance` | scene 系: `ObjectId`。ui: `4294967296 (2^32) + UI 内の instance` (§5) |
| `generation` | instance の世代 (§5) |
| `world` | 4x4 行列 (§3.3)。ui は単位矩形を画面 px の矩形へ写す行列 |
| `screen_bbox` | [x, y, w, h] (px、左上原点、y 下向き)。scene 系は world AABB の 8 隅を投影した外接矩形 (§8)、ui は描画コマンドの矩形 |
| `depth_order` | pass 内の順。0 が最初。scene 系は視点からの深度の近い順 (不透明物の前から後への提出順に相当)、ui は提出順 (0 = 最初に描いた = 最背面) |
| `visibility` | `occlusion-passed` / `occlusion-failed` / `frustum-only` / `unknown` (§6) |
| `alpha` | 最終不透明度 (0..1)。ui は色の a × opacity (1 を超えたら 1)。scene はホストが `FrameTapSceneObject::alpha` を与えたときだけ (無ければ出さない = 契約上 1) |
| `clip` | ui のみ。その draw に効いていた `PushClip` の矩形 (scissor と同じく viewport で切り詰めた値) |
| `ui` | ui のみ。`{role:"bar", element?, fill}` / `{role:"glyph", element?, glyph}`。ホストが与えたときだけ |
| `tags[]` | `unnamed` (アセット名が取れなかった) / `offscreen` (投影結果が画面外) |

### 3.3 行列の並び

Pictor の行列は **行ベクトル規約** (`v' = v * M`、`m[row][col]`、平行移動は `m[3][0..2]`) で、
タップはこれを **行優先** に 16 個並べる。行ベクトル規約の行列を行優先に並べた 16 個は、
同じ変換を **列ベクトル規約で列優先** に並べた 16 個と同じ並びになる (どちらも平行移動が 12〜14 番目)。
したがって契約の「列優先・列ベクトル (要素 12〜14 が平行移動)」とは **値を変えずに一致** する。
右手系・Y 上は Pictor の既定どおり、単位は呼び出し側 (manifest `coordinates.unit`) に従う。

### 3.4 end 行

```json
{"contract":"render-tap/1","seq":3,"end":"shutdown"}
```

- `disable()` / `set_sink()` での差し替え / `PictorRenderer::shutdown()` / `FrameTap` の破棄 → `shutdown`。
- `end_stream(FrameTapEndReason::FAILED)` → `error` (ホストが異常終了を知らせる)。
- end 行の前に開いていたフレームは出さずに捨てる (seq は使ったまま)。end 行無しで流れが閉じたら、
  受信側はそれを切断として扱う (プロセス異常終了など)。
- コールバック配送先は、その参照先がタップより長く生きるか、先に `disable()` すること
  (破棄時にも end 行を渡すため)。

## 4. ID と名前の付け方 (identity)

- **メッシュ**: 次の順で決める。
  1. `register_mesh_data()` の `MeshDataDescriptor::name`、または `FrameTapNameTable::set_mesh_name()` の名前 → `asset-name`。
  2. 名前が無く内容ハッシュがある → `ch:<16 桁 hex>` (頂点バイト列とインデックスバイト列の FNV-1a 64bit、
     各バイト列の長さも混ぜる) → `content-hash`。`register_mesh_data()` は **タップ ON の間に登録された名前の無い
     メッシュ** だけ頂点・インデックスのバイト列から計算する (OFF 時の登録コストを増やさない。後から ON にした
     場合、それ以前に登録したメッシュは count-hash)。ホストは `set_mesh_content_hash()` で与えてもよい。
  3. どちらも無い → `fp:<16 桁 hex>` (頂点数 + インデックス数の FNV-1a 64bit) → `count-hash`。
     衝突するので Commentarii 側では識別に使われない。
  名前の無いメッシュ (2 / 3) は `tags` に `unnamed` を付ける。
- **マテリアル**: Pictor の実行時マテリアルは名前を持たないため、ホストが
  `FrameTapNameTable::set_material_name()` で与える。与えていなければ `material[]` に入れず
  `unnamed` を付け、**draw の `identity` を `count-hash` に下げる** (外見の鍵 mesh + material[] が欠けるので、
  識別に使えない側へ倒す。ハンドル番号は出さない)。
- **UI**: メッシュは描画種別 (`ui:rect` / `ui:nine_slice` / `ui:image`)、マテリアルは
  `FrameTapNameTable::set_ui_texture_name()` で与えたテクスチャ名。純色矩形 (texture 0) は空配列で `asset-name`。
  名前の無いテクスチャはマテリアルと同じく `count-hash` + `unnamed`。

## 5. instance と世代 (generation)

- 受信側は `instance` を pass 横断で照合するので、scene の `ObjectId` (uint32) と ui の instance が重ならないよう、
  ui は `kFrameTapUiInstanceBase` (= 2^32) を足して出す。ui の instance は既定で `UIRenderer` の描画リストの添字、
  ホストが `FrameTapUiAnnotation::instance` で要素 ID を与えればそれを使う。
- 契約: 同じ instance を別の物に使い回すときは `generation` を +1、同じ generation で外見 (mesh + 並べ替えた
  material[]) が変われば違反。Pictor の `ObjectId` は使い回さないが、ui の添字はフレームごとに別の物を指しうる。
  そこで `FrameTapGenerations` が instance ごとに直前の外見のハッシュを覚え、**外見が変わったら世代を上げる**。
  同じ外見の別物への使い回しは外見から区別できないが、契約上の違反にはならない。
- 世代の表は instance を添字にした平坦な配列 (scene / ui の 2 本、タップ ON の間だけ伸びる)。添字が
  `kMaxTrackedInstances` (2^22) 以上の instance は追跡できないので、その draw は `dropped` (other) に数える。
  配送先を差し替えると (新しい流れ) 表を空にする。

## 6. 何を記録するか / 可視性の根拠

- **scene**: static / dynamic プールのうちカリングで可視になった物 (BatchBuilder がバッチにする集合と同じ)。
  GPU-driven プールは GPU 側でカリングするため CPU に可視集合が無く、読み戻しをしない原則により対象外。
  Pictor の CPU カリングは視錐台判定だけなので、可視性の口が無ければ `visibility` は **`frustum-only`**
  (視錐台内だが遮蔽は不明) で、`visibility_lag_frames` は出さない。遮蔽クエリの結果を GPU を待たずに読める
  ホスト (前フレームの結果を持つ等) は `IFrameTapVisibilitySource` を実装して `set_visibility_source()` で渡す。
  そのとき `visibility` はオブジェクトごとにその口へ問い合わせ、`visibility_lag_frames` は口の `lag_frames()`。
- **追加 pass (影・反射・深度 pre-pass 等)**: Pictor 本体は影などの pass を CPU 側の draw 単位で持たないため、
  ホストが `add_pass(name, kind)` で開いて `FrameTapSceneObject::pass` を指定して記録する。
  その pass は 1 フレーム限り (次の `begin_frame()` で scene / ui の 2 pass に戻る)。
- **ui**: `UIRenderer::set_frame_tap()` で渡したタップへ、`record()` が Rect / NineSlice / Image を記録する。
  Text (UIRenderer が描かない) と Push/PopClip は描画ではないので出さない。PushClip の矩形は後続 draw の `clip` になり、
  PopClip で外れる。`screen_bbox` は描画コマンドの矩形そのもの (clip による切り抜きは反映しない — 受信側が `clip` と重ねる)。
  テクスチャ ID は UIRenderer が実際に束縛した値 (範囲外は 0 = 純色) を使う。ui の `visibility` は `unknown`
  (UI は遮蔽クエリの対象外)。HP バーの充填率や数字の文字は、ホストが `record()` の直前に
  `set_frame_tap_annotations(items, count)` で描画リストと同じ添字の `FrameTapUiAnnotation` を渡す
  (`record()` 1 回だけ有効)。UIRenderer が描かない文字 (Text) はホストが `add_ui_draw()` に `role = GLYPH` で直接渡す。
- `render()` より前、または `end_frame()` より後に届いた描画は捨て、`dropped_draws()` に数える (初回のみ stderr に警告)。

## 7. 出せない値の扱い (null を出さない)

契約の schema は数値必須で `null` を許さない。Pictor は次のように扱う。

| 対象 | 条件 | 扱い |
|---|---|---|
| draw | `world` / AABB / `alpha` / UI 矩形 / `clip` に有限でない値 | 出さずにそのフレームの `dropped` (reason `other`) に数える |
| draw | UI 矩形・`clip` の幅か高さが負、`fill` が 0..1 の外 | 同上 (`other`) |
| draw | 存在しない pass 添字 / 世代を追跡できない instance | 同上 (`other`) |
| draw | `set_max_draws_per_frame()` の上限超え | 同上 (`buffer-full`。混在時は buffer-full を優先) |
| draw | `role = GLYPH` で glyph が空 | draw は出し、`ui` だけ出さない |
| draw | `alpha` が 1 超え | 1 に丸める (GPU と同じく飽和) |
| フレーム | viewport が 0、カメラ行列に有限でない値 | 行を出さない (seq は使ったまま = 受信側では欠損)。`dropped_frames()` に数える |
| フレーム | 開いたまま次の `begin_frame()` / 配送先の差し替え / end | 同上 |
| `t` / `tick` | 有限でない・負・逆行 | 前の行の値に留める |

## 8. screen_bbox の求め方

world AABB の 8 隅を `view * projection` でクリップ空間へ送り、視点の前 (w > ε) にある隅と、
前後をまたぐ 12 辺と w = ε 面の交点を射影して外接矩形を取る。頂点は走査しない。
結果は viewport で切り詰め、面積が無ければ `[0,0,0,0]` と `offscreen` タグ。

## 9. 検証 (Commentarii の schema / golden)

`unit_frame_tap_test` は固定シーン (メッシュ 2 + UI 矩形 1 + 影 pass 1、2 フレーム + end 行) の出力を
作業ディレクトリ (CTest では build ディレクトリ) の `frame_tap_fixed_scene.jsonl` に書く。
`tools/frame_tap/validate-render-tap.mjs` はそれと golden を schema で検証し、golden と同じ流れの形
(seq が 0 から 1 ずつ、最後が end 行、frame 単調増加、t / tick が減らない、observer あり) を確かめる。
Pictor は node 依存を持たないので、Ajv は `--ajv-from` で渡した場所 (Commentarii の checkout 等) から読む。

```bash
node tools/frame_tap/validate-render-tap.mjs \
  --schema  <Commentarii>/schema/render-frame.schema.json \
  --golden  <Commentarii>/tests/fixtures/render-tap/golden-v1.jsonl \
  --ajv-from <Commentarii> \
  <build>/frame_tap_fixed_scene.jsonl
```

## 10. 構成 (1 ファイル 1 責務)

| ファイル | 責務 |
|---|---|
| `frame_tap_vocabulary.h/.cpp` | 契約の版・列挙値 (pass の種類 / 可視性 / identity / UI の役割 / end / dropped の理由) と文字列、観測者 ID の書式 |
| `frame_tap_types.h` | 1 フレーム分の記録の値型 (ヘッダのみ) |
| `frame_tap_sink.h/.cpp` | 1 行の配送先 (stdout / ファイル追記 / コールバック) |
| `frame_tap_config.h/.cpp` | `PICTOR_FRAME_TAP` 書式の解釈と配送先の生成 |
| `frame_tap_asset_source.h` | アセット名の問い合わせ口 (interface) |
| `frame_tap_visibility_source.h` | 可視性の根拠の問い合わせ口 (interface) |
| `frame_tap_name_table.h/.cpp` | ホストが与える名前表 (interface の既定実装) |
| `frame_tap_asset_ids.h/.cpp` | 名前 / 内容ハッシュ / 指紋からの安定 ID と identity、外見のハッシュ |
| `frame_tap_clock.h/.cpp` | `t` / `tick` (ホストの値か代わりの値、減らない) |
| `frame_tap_generations.h/.cpp` | instance の世代 (外見が変わったら +1) |
| `frame_tap_projection.h/.cpp` | AABB の画面投影と視点深度 |
| `frame_tap_json_line.h/.cpp` | 1 フレーム / end を 1 行の JSON にする |
| `frame_tap.h/.cpp` | 流れ (seq / end)・フレームの開閉・draw の記録と検査・深度順の確定・配送 |
| `scene_frame_tap_collector.h/.cpp` | SceneRegistry の可視オブジェクトをタップへ流す |
| `tools/frame_tap/validate-render-tap.mjs` | 出力を Commentarii の schema / golden で検証する開発用スクリプト |

`PictorRenderer` は `frame_tap()` と `frame_tap_names()` を公開し、`register_mesh_data()` /
`unregister_mesh_data()` のたびにメッシュ名と頂点数・インデックス数 (タップ ON なら名前の無いメッシュの
内容ハッシュも) を名前表へ写す (登録時に 1 回だけ。フレームごとのコストは無い)。

## 11. 復旧

タップは観測専用で描画結果を変えない。不具合時は `PICTOR_FRAME_TAP` を外す
(または `RendererConfig::frame_tap` を空にする) と、OFF 時の 1 分岐以外は変更前と同じ経路になる。
