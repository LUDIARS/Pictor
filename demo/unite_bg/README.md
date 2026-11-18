# pictor_unite_bg_demo

Unite Tokyo 2026 の講演「『hololive Dreams』の背景づくりと 24 時間ライティング」（QualiArts）で紹介された手法を、Pictor（Vulkan）上で再現した学習用デモです。
解説とクイズはこのリポジトリには置かず、別のドキュメントにまとめています。このフォルダにあるのは **コードとシェーダーだけ** です。

## ビルドと実行

```bash
cmake -B build            # PICTOR_BUILD_DEMO=ON (既定), Vulkan SDK + GLFW
cmake --build build --target pictor_unite_bg_demo
cd build                  # shaders/unite_bg/*.spv と shaders/text_overlay.*.spv を相対パスで読む
./pictor_unite_bg_demo    # MSVC の場合は build から Debug\pictor_unite_bg_demo.exe を起動
```

起動オプション: `--scene N`（1〜9 のシーンから始める） / `--frames N`（N フレーム描いたら終了）

## 操作

| キー | 動作 |
|---|---|
| 1〜9 | シーン切替（時刻・カメラ・露出はシーンの既定値に戻る） |
| ↑ / ↓ | パラメータを選ぶ |
| ← / → | 選んだパラメータを増減（Shift で 4 倍）。整数パラメータは PgUp / PgDn も使える |
| Z X C V N M | シーンごとのトグル（HUD に表示） |
| [ / ] | SkyTime を戻す / 進める（1 秒で 1 時間、Shift で 4 時間） |
| T | 時刻の自動再生（速さは Scene 9 の Time Speed） |
| - / = | 露出 |
| 右ドラッグ / WASD / Q E | カメラの向き / 移動（Shift で速く） |
| B | ベイクをやり直す（雲 SixWay・SkyOcclusion・LocalGI） |
| G | （Scene 8）建物の状態を切り替える: なし → 建築中 → 完成 |
| R | トグルを初期値に戻す |
| H | HUD の表示 / 非表示 |

## シーンとシェーダー

| # | 内容 | シェーダー |
|---|---|---|
| 1 | 大気散乱（レイリー / ミー、波長、Profile の Tint） | `sky.frag`, `atmosphere.glsl` |
| 2 | 夜空（星 × ノイズの UV スクロールによる瞬き、回転、天の川、Profile のカーブ） | `night.frag` |
| 3 | 空気感（霞 = 空気遠近）と遮蔽判定（深度レイマーチによるライトシャフト） | `shaft_scene.frag` → `shaft_composite.frag` |
| 4 | 海（Gerstner 波、浅瀬で波を弱める、水底の色を抑える、船の水抜き、コースティクス、LOD 表示、CPU 浮力） | `ocean.frag`, `gerstner_cpu.h` |
| 5 | 雲（SixWay ベイク、前方散乱、雲専用ライト） | `cloud_bake.frag` → `clouds.frag` |
| 6 | GI（SkyOcclusion × GlobalGI、地面側の上書き、LocalGI、優先度、ベイクの限界） | `gi_probe_bake.frag`, `gi_local_bake.frag` → `gi.frag`, `gi_scene.glsl` |
| 7 | タレントのライティング（専用ライト 3 モード、輪郭リム、GI 補正） | `talent.frag` |
| 8 | 草（セル分割 + 連続セルの Dispatch 統合 + ComputeShader カリング + DrawIndirect、風、踏み分け、建物 Collider） | `grass_env.comp`, `grass_cull.comp`, `grass.vert/.frag`, `grass_ground.frag`, `grass_system.cpp` |
| 9 | SkyTime と Profile（グラデーション / カーブ、固定値の切替、Timeline クリップブレンド） | `profile.frag`, `sky_profile.cpp` |

## 講演との違い（簡略化した点）

- 背景のメッシュは使わず、SDF（距離関数）をレイマーチして描いています。草だけは本物のメッシュ（手続き生成の板ポリ）で、GPU カリングと間接描画を使います。
- 海のジオメトリ（20m タイルの分割）は作っていません。代わりに LOD の区切り方を色で表示します（V キー）。
- APV のプローブは均一なグリッドで、L1 球面調和に空の見え具合を焼いています（本家はジオメトリの近くほど細かい Adaptive 配置）。
- DynamicReflectionProbe は Cubemap と mip の代わりに、6 方向それぞれを円錐内のレイの平均で作っています（Ambient Cube）。
- 草の配置データは Houdini の代わりに、ノイズと道のマスクから手続き生成しています。
