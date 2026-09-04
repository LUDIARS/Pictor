# 影絵デモ — 切り絵バックドロップ

対象: `demo/shadow_play/sp_kirie_backdrop.{h,cpp}`、`demo/shadow_play/shaders/sp_cutout.glsl`、
`demo/shadow_play/shaders/sp_screen.frag`、`demo/shadow_play/shaders/sp_godray.frag`。
関連: `spec/tasks/2026-08-27-shadow-cutout-godray.md` (月光カットアウトとゴッドレイ)。

Figmentum-kirie で生成した切り絵サンプル 2 枚を、障子スクリーンの映像へ合成する。
街並みはスクリーン全面の背景として、鷹は月の丸穴の中のシルエットとして使う。

## 切り絵バックドロップ {#SPEC-SHADOW-KIRIE-BACKDROP}

### 読み込み

`SpKirieBackdrop::initialize(vk_ctx, asset_dir)` が `asset_dir` 直下の 2 枚を
Vulkan テクスチャとして読む。

| ファイル | 用途 | 台紙キーイング |
|---|---|---|
| `kirie_town.png` | スクリーン全面の背景 | しない (不透明のまま) |
| `kirie_hawk.png` | 月の丸穴に置く切り抜き | **する** |

サンプラは linear filter / `CLAMP_TO_EDGE` の 1 個を両テクスチャで共有する。

### 台紙キーイング

`key_out_background()` が、切り絵の紙地 (台紙) を色距離でアルファへ変換する。

- **背景色は四隅 4 点の平均**とみなす。切り絵サンプルは紙地の上に切り絵を置いた構図なので、
  四隅は必ず台紙になる。
- 各ピクセルの背景色からの RGB ユークリッド距離 `dist` を求め、
  `alpha = (dist - t0) / (t1 - t0)` を `[0,1]` にクランプする。
- 閾値は **`t0 = 50` で完全透明、`t1 = 85` で不透明**、その間は線形。
  これは gen08 サンプルの実測値。**素材を差し替えるときは再計測する。**
- 鷹本体とは別のピースが写り込んでいる画像下端 12% (`y >= 0.88 * height`) は、
  キーイング後にアルファを 0 へ上書きする。素材を差し替えるときはこの範囲も再確認する。

### 街バックドロップの夜風グレーディング

`sp_screen.frag` が街テクスチャを障子の紙地へ焼き込む。元の切り絵は昼の色なので、
そのままでは夜の影絵に馴染まない。次の順で夜へ寄せる。

1. 輝度 `town_luma` を求め、`mix(vec3(town_luma), town, 0.40)` で **彩度を 40% まで落とす**
2. `vec3(0.52, 0.62, 0.92)` を掛けて **青へ寄せる**
3. `pow(town, 1.35)` で **暗部を沈める**
4. 合成量は `town_sample.a * 0.82 * (1.0 - in_moon)` — **月の丸穴の中では街を出さない**
   (穴は月光の通り道であって背景ではないため)

### 鷹の月内シルエット

`sp_hawk_filter(sheet_p, hawk_tex)` が月の丸穴を通る光の透過率を返す。
この透過率は障子スクリーンと前方のゴッドレイの両方へ適用する。

- 穴の外 (`dot(local, local) >= 1.0`) は `vec3(1.0)` = 素通し。丸穴の中だけが対象。
- 穴ローカル座標は `(sheet_p - SP_MOON_CENTER) * vec2(SP_SHEET_ASPECT, 1.0) / SP_MOON_RADIUS`
  で `-1..1` へ正規化する。シート縦横比を掛けるのは、丸穴を楕円にしないため。
- 鷹は **横を穴の直径の約 63% に収め** (係数 `1.6`)、縦は画像アスペクト `1024/768` を保つ。
  月の中で余白を持って浮かせるための値で、全幅いっぱいにすると月ではなく額縁に見える。
- 透過は `mix(vec3(1.0), hawk.rgb * 0.05, hawk.a)`。**切り絵の紙は月光をほぼ遮り**、
  わずかな暖色の透けだけを残す。

月の位置と大きさ (`SP_MOON_CENTER` / `SP_MOON_RADIUS`) と光芒定数 (`SP_BEAM_*`) は
`sp_cutout.glsl` に集約されており、このドキュメントでは値を二重に持たない。

### フォールバック

**PNG が読めなくてもデモは動き続ける。**

- どちらかの読み込みが失敗したら、失敗した側だけを **1x1 の白 (α=0)** テクスチャへ差し替える。
- α=0 なので、街の合成量 `town_sample.a * ...` は 0、鷹の `mix(..., hawk.a)` も素通しになり、
  **背景合成が no-op になるだけで本体の影絵演出は出る**。
- デスクリプタは常に有効なまま保つ。「テクスチャが無いからバインドしない」分岐を作らない。
- 失敗時は stderr へ `SpKirieBackdrop: assets not found under <dir> (town=N hawk=N)` を出す。

`initialize` が `false` を返すのは **フォールバック生成すら失敗したとき**、
device / asset_dir が無いとき、サンプラ生成に失敗したときだけ。

### 破棄

`shutdown()` は image view → image → memory の順で解放し、サンプラを破棄したあと
全ハンドルを `VK_NULL_HANDLE` へ戻す。`initialize` の先頭でも呼ぶので、
**二重初期化しても資源が漏れない**。
