# XR モジュール (VR 対応・差分レンダリング)

2026-09-18。 任意モジュール `Pictor::xr`。 ドメインは `spec/domains/xr.domain.json`。

## 何ができるか

HMD (まず Meta Quest を PC へ Link 接続したもの) に Pictor の画を出す。 観客は椅子に
座り、 頭を動かして周囲を見渡せる。 最初の用途は VR 映画で、 背景の大半が動かない。
その性質を使い、 描き直す画素を減らす (差分レンダリング)。

host が得るもの:

- 頭と両手のコントローラの姿勢・入力値 (意味付けは host。 PC-RULE-001)
- 頭に追従する左右の描画カメラと、 カリング用のカメラ 1 つ
- HMD へ提出する描画先 (2 層の画像。 層 0 = 左眼、 層 1 = 右眼)
- 差分レンダリングの計画 (どの眼を全面描画し、 どの眼を再投影で済ませるか) と、
  再投影を GPU で行うパス

## モジュールの構成と選び方

| build option | 既定 | 中身 | 外部依存 |
|---|---|---|---|
| `PICTOR_ENABLE_XR` | ON | 両眼カメラ、 カリング視錐台、 再投影の行列、 差分描画の計画、 名前付き既定値、 再投影パス (Vulkan があるとき) | なし |
| `PICTOR_ENABLE_OPENXR` | OFF | OpenXR ランタイムとの接続、 HMD 用の描画先、 コントローラ入力、 VR demo | OpenXR loader、 Vulkan |

- OpenXR loader は、 インストール済みの package があればそれを使い、 無ければ
  `PICTOR_OPENXR_SDK_TAG` (既定 `release-1.1.63`) を FetchContent で取得する。
- `PICTOR_ENABLE_OPENXR=ON` なのに `PICTOR_ENABLE_XR=OFF`、 または Vulkan が無い構成は
  configure を FATAL_ERROR で止める (黙って無効化しない)。
- `PICTOR_ENABLE_XR=OFF` では XR の実装・shader 生成・test・demo を一切要求しない。
- core (`pictor`) は XR の実装を参照しない。 core に足したのは XR に限らず使える部品だけ:
  `core/camera.h` (`Camera` の切り出し)、 `core/transform_math.h` (行列と姿勢の演算)、
  `culling/frustum_utils.h` (既存の視錐台抽出の公開)、
  `surface/vulkan_device_requirements.h` (デバイス生成への外部要求の口)。

対応 backend は Vulkan のみ。 DX12 / Metal / WebGL では OpenXR 接続と再投影パスは使えない
(CPU 側の計画とカメラは backend に依らず使える)。

## SDK の選択

Oculus 対応は OpenXR で行う。 旧 Oculus PC SDK (LibOVR) は Meta が非推奨にしており、
Meta の現行の公式経路は OpenXR。 Quest 単体 (Android) へも同じコードで届く。
Vulkan との結合は `XR_KHR_vulkan_enable` を使う。 ランタイムが「必要な拡張」 と「使うべき
GPU」 を返し、 インスタンスとデバイスの生成は `VulkanContext` が主導したまま行える
(`vulkan_enable2` はランタイムに生成を委ねる方式で、 既存の生成経路を迂回してしまう)。

## 初期化の順序 {#SPEC-PC-XR-OPENXR-SESSION}

1. `OpenXrRuntime::create()` — ランタイムへ接続し HMD を見つける
2. `VulkanContext::initialize()` — `VulkanContextConfig::device_requirements` に runtime を渡す。
   要求された拡張が無い・GPU を取得できない場合は初期化を失敗させる
3. `OpenXrRuntime::start_session()` — Vulkan と結び付け、 描画先と入力を用意する

破棄は逆順。 runtime は HMD の画像への view を持つので VkDevice より先に壊す。

## 1 フレームの流れ

```
session.poll_events()
session.begin_frame(timing)            … 表示予定時刻を受け取る
session.locate_views(timing, views)    … その時刻の両眼の姿勢と視野
session.sample_input(timing, input)
camera.update(views)                   … 左右のカメラ + カリング用カメラ
plan = planner.plan(left, right)       … 差分描画の計画
surface.acquire(image_index) → 描画 → surface.release()
session.end_frame(timing, views, has_layer)
```

基準空間は LOCAL (起動時の頭の位置が原点。 座った姿勢向け)。 観客の立ち位置は
`CameraRig::world_from_tracking` で host が決める。 トラッキングが切れたフレームは
`StereoCamera::update()` が false を返し、 直前のカメラを保つ (画が原点へ飛ばない)。

## カリング

両眼の視錐台をまとめて包む視錐台を 1 つ作り、 カリングは 1 回で済ませる。 左眼の座標系で
視点を両眼の中点から後ろへ `両眼の距離 / (2 × 外向きの視野の正接)` だけ下げ、 両眼の
視錐台の頂点 16 個がすべて入るまで視野を広げる。 視錐台は凸なので、 頂点が入れば全体が入る
(見えるものを落とさない)。 表示面が内外へ傾いた HMD でも成り立つ。

## 差分レンダリング {#SPEC-PC-XR-DELTA-RENDERING}

対象: `src/xr/reprojection.cpp`、 `src/xr/reprojection_pass.cpp`、 `src/xr/delta_render_planner.cpp`、
`shaders/xr/reproject_warp.vert`、 `shaders/xr/reproject_warp.frag`。

物体を「静的層」 と 「動的層」 に分けるのは host の責務。 静的層だけを使い回し、 動的層は
毎フレーム上から描く。

### 共通の仕組み: 再投影 + 深度による穴埋め

1. 描画済みの静的層 (色 + 深度) を、 格子メッシュの変形で今の視点へ写す
   (`ReprojectionPass`)。 各頂点が元の深度を読み、 行列 1 つ (`build_reprojection()`) で
   写し先のクリップ座標へ移る。 遮蔽はラスタライザの深度テストが解決する。
2. 元の視点から見えていなかった面をまたぐ三角形は大きく引き伸ばされる。 フラグメント
   シェーダが「写し先 1 画素あたり元の画を何画素進むか」 を見て、 小さければ捨てて穴にする。
3. 同じ render pass のまま、 静的層の物体を **深度テスト LESS** で描く。 写せた画素は
   少し手前の深度を持つので弾かれ、 穴の画素だけがシェーディングされる。 host の描画コードは
   通常の描画のままでよく、 ステンシルやマスクは要らない。

見る向きで見え方が変わる材質 (強い鏡面・屈折) は再投影すると破綻する。 それらは動的層へ
入れる (毎フレーム描く)。

### 両眼差分

左眼の静的層を描いてキーフレームに取り、 右眼はそこから再投影して穴だけ描く。 視差は被写体が
近いほど大きく、 穴が増える。 穴の割合が `max_hole_ratio` を超えたら、 `eye_delta_cooldown`
フレームのあいだ右眼を全面描画へ戻す。

### 時間差分

眼ごとにキーフレーム (静的層を全面描画した時の色・深度・カメラ) を持ち、 以降のフレームは
キーフレームから今の視点へ再投影する。 毎回キーフレームから写すので、 再投影のぼけは
フレームをまたいで累積しない。 次のどれかでキーフレームを取り直す:

| 理由 | 既定の閾値 |
|---|---|
| 頭がキーフレームの位置から離れた | 0.10 m |
| 視線がキーフレームの向きから回った | 0.35 rad (約 20 度) |
| キーフレームが古い | 90 フレーム |
| 穴埋めで描いた画素が多すぎた | 全画素の 25% |
| host が静的層の変更を通知した | `notify_static_scene_changed()` |

### 名前付き既定値 (PC-RULE-003) {#SPEC-PC-XR-STEREO-PRESETS}

`find_stereo_preset(id)` で選び、 返った `DeltaRenderConfig` を上書きして使う。
demo も host も同じ入口を使う。 未知の id は nullptr (別の既定値へ置き換えない)。

| id | 方式 |
|---|---|
| `vr.multiview` | 1 パスで両眼を全面描画 (VK_KHR_multiview が必要)。 差分なしの基準 |
| `vr.two_pass` | 眼ごとに全面描画 |
| `vr.eye_delta` | 両眼差分のみ |
| `vr.film_delta` | 両眼差分 + 時間差分。 VR 映画向け |

multiview は 1 回の描画が両眼へ走るので、 再投影を挟む余地が無い。 差分描画とは排他。

## demo {#SPEC-PC-XR-DEMO}

対象: `demo/vr_openxr/main.cpp`、 `vr_eye_targets.cpp`、 `vr_scene_renderer.cpp`、 `vr_vulkan_util.cpp`、
`demo/vr_openxr/shaders/vr_scene.vert`、 `vr_scene_multiview.vert`、 `vr_scene.frag`。

`pictor_vr_openxr_demo [preset]` — 箱で組んだ部屋の中央の椅子に座り、 周囲を見渡す。
机の上を回る箱と上下に揺れる箱が動的層。 300 フレームごとに穴の割合を出力する。
PC 画面への鏡表示はしない。 フレームごとに GPU の完了を待つ単純な同期にしてある。

## 確認できていること / いないこと

- 確認済み: Windows + MSVC で `PICTOR_ENABLE_OPENXR=ON` の build が通る。
  `PICTOR_ENABLE_OPENXR=OFF` / `PICTOR_ENABLE_XR=OFF` の configure が通り、 不正な組合せは止まる。
  単体テスト 4 本 (`unit_transform_math_test` / `unit_xr_stereo_camera_test` /
  `unit_xr_reprojection_test` / `unit_xr_delta_planner_test`) と既存の `unit_culling_test` が通る
  (2026-09-18、 Debug、 ユーザーの明示許可のもとで実行)。
- 未確認: HMD での表示、 再投影の見た目と実際の穴の割合、
  GPU 負荷の削減量。 実行はユーザー側で行う (Pictor の作業ルール)。
- 未対応: Quest 単体 (Android) のビルド、 PC 画面への鏡表示、 差分描画と後処理
  (TAA 等) の併用、 既存の `PictorRenderer::render()` の両眼化
  (現状は host が眼ごとにカメラを渡して描く)。
