# Pictor demo別 描画機能・プラットフォーム対応表

更新: 2026-10-15。機械可読の正本: `spec/data/demo-platform-matrix.json`。
機能別の対応表 `spec/feature/platform-feature-matrix.md` で PC-FEAT-TOOLS-01 へまとめていた
毛皮・縄・涙・影絵などのdemo専用表現を、demo単位へ展開したもの。
参照snapshotは `3ecaa23` の `CMakeLists.txt`、`cmake/*.cmake`、`demo/**`。

## 方針

- 各demoは固有パイプラインを持つ独立した設計として記載する。デフォルトとして提供する場合もdemoごとの選択肢であり、demo間で共有・統合した万能パイプラインへまとめない。
- 「ライブラリ機能」列は Pictor ライブラリの汎用機能で、既存の機能ID(PC-FEAT-*)に関連付ける。「demo専用」はdemo配下だけにある表現で、ライブラリAPIではない。
- ソース経路、ビルド確認、実機確認は別の列に分ける。ソース経路があってもビルド成功・実機動作を意味しない。
- 対応表の作成だけを行い、描画モジュールの移植・ビルド・起動テストはしていない。

## 凡例

- VK: Vulkan+GLFW前提のCMake targetが作られるソース経路がある。
- VK※: macOSはVulkan提供環境(MoltenVK等)とGLFWが別途必要。ネイティブMetalではない。
- CPU: GPU API非依存のheadless実行経路。Windowsでは描画backendを使わないため両列に記す。
- 条件: 既定OFFのオプション、外部prebuilt・ランタイム、glslc、任意モジュールが揃う場合だけtargetが作られるか、機能する。
- Web: Emscripten toolchainでだけtargetが作られる。
- APK: `demo/android/CMakeLists.txt` の別CMakeプロジェクト。
- 未接続: demoソースはあるがCMake targetが無い。
- 対象外: このdemoのtargetが作られない、または該当経路を持たない。
- 本体の `CMakeLists.txt` は `PICTOR_MOBILE` のとき demo群を丸ごとスキップする。このためAndroid/iOS列は、別プロジェクトのAndroid APKを除いてすべて対象外。iOS向けのdemoは無い。

## ソース経路

| ID / demo (target) | Web/WebGL2 | Windows/Vulkan | Windows/DX12 | Linux/Vulkan | macOS/Vulkan | Android/Vulkan | iOS/Metal |
|---|---|---|---|---|---|---|---|
| PC-DEMO-01 100万球ベンチマーク (`pictor_benchmark`) | 対象外 | VK/CPU | CPU | VK/CPU | VK※/CPU | 対象外 | 対象外 |
| PC-DEMO-02 FBX読込・アニメーション (`pictor_fbx_demo`) | 対象外 | CPU | CPU | CPU | CPU | 対象外 | 対象外 |
| PC-DEMO-03 モバイル描画プロファイル (移設済み → ErgoDemo `ergodemo_mobile`) | 移設 | 移設 | 移設 | 移設 | 移設 | 移設 | 移設 |
| PC-DEMO-04 文字エフェクトsmoke (`pictor_text_effects_demo`) | 対象外 | CPU | CPU | CPU | CPU | 対象外 | 対象外 |
| PC-DEMO-05 マテリアルJSON往復smoke (`pictor_material_serializer_demo`) | 対象外 | CPU | CPU | CPU | CPU | 対象外 | 対象外 |
| PC-DEMO-06 FBXビューア 毛皮・縄・涙 (`pictor_fbx_viewer`) | 対象外 | VK | 対象外 | VK | VK※ | 対象外 | 対象外 |
| PC-DEMO-07 PN多項式曲面 (`pictor_pn_demo`) | 対象外 | VK | 対象外 | VK | VK※ | 対象外 | 対象外 |
| PC-DEMO-08 Vulkanウィンドウ (`pictor_demo`) | 対象外 | VK | 対象外 | VK | VK※ | 対象外 | 対象外 |
| PC-DEMO-09 PBR・CSM影・SSAO (移設済み → ErgoDemo `ergodemo_graphics`) | 移設 | 移設 | 移設 | 移設 | 移設 | 移設 | 移設 |
| PC-DEMO-10 海面テッセレーション (移設済み → ErgoDemo `ergodemo_ocean`) | 移設 | 移設 | 移設 | 移設 | 移設 | 移設 | 移設 |
| PC-DEMO-11 影絵 (移設済み → ErgoDemo `ergodemo_shadow_play`) | 移設 | 移設 | 移設 | 移設 | 移設 | 移設 | 移設 |
| PC-DEMO-12 文字描画 (`pictor_text_demo`) | 対象外 | VK | 対象外 | VK | VK※ | 対象外 | 対象外 |
| PC-DEMO-13 ポストエフェクト (移設済み → ErgoDemo `ergodemo_postprocess`) | 移設 | 移設 | 移設 | 移設 | 移設 | 移設 | 移設 |
| PC-DEMO-14 VR/OpenXR (`pictor_vr_openxr_demo`) | 対象外 | 条件VK | 対象外 | 条件VK | 対象外 | 対象外 | 対象外 |
| PC-DEMO-15 Rive再生 (`pictor_rive_demo`) | 対象外 | 条件VK | 対象外 | 条件VK | 条件VK※ | 対象外 | 対象外 |
| PC-DEMO-16 2Dテクスチャ (移設済み → ErgoDemo `ergodemo_texture2d`) | 移設 | 移設 | 移設 | 移設 | 移設 | 移設 | 移設 |
| PC-DEMO-17 SHaRC拡張 Vulkan (`pictor_sharc_demo`) | 対象外 | 条件VK | 対象外 | 条件VK | 条件VK※ | 対象外 | 対象外 |
| PC-DEMO-18 SHaRC D3D12 headless (`pictor_sharc_dx12_test`) | 対象外 | 対象外 | 条件DX12 | 対象外 | 対象外 | 対象外 | 対象外 |
| PC-DEMO-19 関係グラフ・dock (`pictor_graph_demo`) | 対象外 | VK | 対象外 | VK | VK※ | 対象外 | 対象外 |
| PC-DEMO-20 WebGL2 (`pictor_webgl_demo`) | Web | 対象外 | 対象外 | 対象外 | 対象外 | 対象外 | 対象外 |
| PC-DEMO-21 Androidデモ集APK (`pictor_demos`) | 対象外 | 対象外 | 対象外 | 対象外 | 対象外 | APK | 対象外 |
| PC-DEMO-22 Riveキューブ (targetなし) | 対象外 | 未接続 | 対象外 | 未接続 | 未接続 | 対象外 | 対象外 |

`pictor_web_mesh` (`cmake/PictorWebGL.cmake`) はWeb向けメッシュAPIのexportでdemoではないため載せない。`tools/` の制作ツールもdemoではないため対象外。

## ビルド確認・実機確認

| ID | ビルド確認 | 実機確認 |
|---|---|---|
| PC-DEMO-01〜20 | 未確認(この対応表作成ではビルドしていない。過去PRのTest OKを転用しない) | 未確認。PC-DEMO-04/05は表示を伴わないため対象外 |
| PC-DEMO-21 | 成功記録あり: SDK 31 / build-tools 34.0.0 / NDK 21.4.7075529 / JDK 11 / CMake 4.1.2でAPK生成・Debug署名検証まで(`android-demo-collection.md`、Android互換修正を適用したローカルmain基点)。47967d8のAndroid全体ビルド失敗とは基点が異なる。今回は再ビルドしていない | 未確認。初回描画・背景復帰・surface再生成は端末接続後に別途確認する |
| PC-DEMO-22 | 対象外(targetが無い) | 未確認 |

## demoごとの機能・依存・固有パイプライン

### PC-DEMO-01 100万球ベンチマーク — `pictor_benchmark`

- 依存: `pictor`。Vulkan/GLFWが無くてもheadlessで更新だけ回す。
- ライブラリ機能: PC-FEAT-DOD-01、PC-FEAT-DOD-06、PC-FEAT-DOD-07、PC-FEAT-PLAT-01
- demo専用: Lissajous軌道の100万球更新シナリオ(GPU compute / `--cpu`)。
- 固有パイプライン: ObjectDescriptorを大量登録し、SimpleRendererでインスタンス描画。GPU compute更新は DOD-07 (GPUDrivenPipeline::execute未実装) と同じ完成度とは扱わない。

### PC-DEMO-02 FBX読込・アニメーション — `pictor_fbx_demo`

- 依存: `pictor`、headless。
- ライブラリ機能: PC-FEAT-DRAW-14、PC-FEAT-DRAW-15
- 固有パイプライン: 描画なし。FBX読込とAnimationSystemの評価結果をconsoleへ出力。

### PC-DEMO-03 モバイル描画プロファイル — `pictor_mobile_demo`

- **移設 (2026-10-27)**: [LUDIARS/ErgoDemo](https://github.com/LUDIARS/ErgoDemo) の `demos/mobile/` (target `ergodemo_mobile`) へ移した。Pictor ではビルドしない。以下は移設時点の記述で、Ergo でのパラメータ調整は ErgoDemo 側の文書を正とする。

- 依存: `pictor`、headless。POST_BUILDで `demo/mobile/profiles/mobile_custom.ini` を配置。
- ライブラリ機能: PC-FEAT-DOD-01、PC-FEAT-PIPE-01、PC-FEAT-PLAT-04
- 固有パイプライン: GPU submitなし。MobileLow/MobileHighとPipelineProfileBuilderの設定を切り替え、lifecycle・thermal・memory pressureをdesktop hostで再現。
- 制限: 名称に反し `PICTOR_MOBILE` ビルドでは作られない。モバイル実機挙動の確認ではない。

### PC-DEMO-04 文字エフェクトsmoke — `pictor_text_effects_demo`

- 依存: `pictor`、headless。ライブラリ機能: PC-FEAT-DRAW-11
- 固有パイプライン: 合成alpha/pathへ全エフェクトを適用しexit codeで判定。

### PC-DEMO-05 マテリアルJSON往復smoke — `pictor_material_serializer_demo`

- 依存: `pictor`、headless。ライブラリ機能: PC-FEAT-DRAW-01
- 固有パイプライン: MaterialDescのJSON出力・再読込・全field比較。

### PC-DEMO-06 FBXビューア — `pictor_fbx_viewer`

- 依存: `pictor`、Vulkan+GLFW、stb。
- ライブラリ機能: PC-FEAT-PLAT-01、PC-FEAT-DRAW-14、PC-FEAT-DRAW-15、PC-FEAT-DOD-08
- demo専用:
  - シェル毛皮: `demo/fbx_viewer/fur_shell_pass.cpp`、`shaders/fur_shell.*`
  - 縄の巻き付け変形・縄ジオメトリ: `nawa_binding.cpp`、`rope_pass.cpp`
  - 涙と目位置推定: `tear_pass.cpp`、`eye_locator.cpp`
  - フレームキャプチャ・swapchain readback・raw出力: `frame_capture.cpp`、`swapchain_readback.cpp`、`raw_frame_writer.cpp`
  - CSVトラック再生・morph target: `track_player.cpp`、`track_csv.cpp`、`morph_targets.cpp`
- 固有パイプライン: demo独自のskinning(model/bone shader)に任意パスを重ねる。毛皮・縄・涙はCLIかキー入力で有効化するまで無効(`spec/tasks/2026-09-02-fbx-viewer-fur-effects.md`)。PNソースもリンクされるが `PICTOR_PN_DEMO` 未定義のため無効。
- 実機確認には手元で用意したモデルが必要。非公開アセットはリポジトリに含めない。

### PC-DEMO-07 PN多項式曲面 — `pictor_pn_demo`

- 依存: `pictor`、Vulkan+GLFW。`PICTOR_BUILD_POLYNOMIAL_SDK=ON` で `pictor_polynomial_demo` 静的ライブラリも作る(Windows desktop、Rive/PCM無効に限る)。
- ライブラリ機能: PC-FEAT-PLAT-01、PC-FEAT-DRAW-14、PC-FEAT-DRAW-15、PC-FEAT-DOD-08
- demo専用: PN多項式の有界sphere tracing(`spec/feature/pn-raymarch.md`)、PN patch再構成(`pn-reconstruction.md`)、多項式モーション接続・録画・HUD(`polynomial-motion.md`)。
- 固有パイプライン: `PICTOR_PN_DEMO=1` でfbx_viewerのソースを別targetとしてコンパイルし、既定はraymarch。F5で明示的にraster参照へ切り替え、自動fallbackはしない。
- `include/pictor/demo/polynomial_motion.h` はheader-onlyのdemo adapterで、ライブラリの汎用描画機能ではない。fbx_viewerとのソース共有はビルド構成上の再利用で、パイプラインはtargetごとに別。

### PC-DEMO-08 Vulkanウィンドウ — `pictor_demo`

- 依存: `pictor`、Vulkan+GLFW。ライブラリ機能: PC-FEAT-PLAT-01、PC-FEAT-DOD-01、PC-FEAT-DOD-08
- 固有パイプライン: clear colorの色相アニメーションとstats overlay。frame resultの再初期化要求を扱う。

### PC-DEMO-09 PBR・CSM影・SSAO — `pictor_graphics_demo`

- **移設 (2026-10-27)**: [LUDIARS/ErgoDemo](https://github.com/LUDIARS/ErgoDemo) の `demos/graphics/` (target `ergodemo_graphics`) へ移した。Pictor ではビルドしない。以下は移設時点の記述で、Ergo でのパラメータ調整は ErgoDemo 側の文書を正とする。

- 依存: `pictor`、Vulkan+GLFW。
- ライブラリ機能: PC-FEAT-PLAT-01、PC-FEAT-DOD-01、PC-FEAT-DRAW-02、PC-FEAT-DRAW-03、PC-FEAT-DRAW-04、PC-FEAT-DRAW-05
- 固有パイプライン: demo独自のPBR shader(`pbr_demo.*`)に、ライブラリのGIConfig(CSM 3段・SSAO有効・probe無効)を設定。影・SSAO・GIの完成度は機能別対応表の各行に従う。

### PC-DEMO-10 海面テッセレーション — `pictor_ocean_demo`

- **移設 (2026-10-27)**: [LUDIARS/ErgoDemo](https://github.com/LUDIARS/ErgoDemo) の `demos/ocean/` (target `ergodemo_ocean`) へ移した。Pictor ではビルドしない。以下は移設時点の記述で、Ergo でのパラメータ調整は ErgoDemo 側の文書を正とする。

- 依存: `pictor`、Vulkan+GLFW。実行にはtessellation shader対応GPUが必要。
- ライブラリ機能: PC-FEAT-PLAT-01
- demo専用: 距離適応テッセレーション・Gerstner波・海面shading(`ocean.tesc/tese/frag`)。該当する機能IDは無い。
- macOS/MoltenVK等でのtessellation可否は条件付き(未確認)。

### PC-DEMO-11 影絵 — `pictor_shadow_play_demo`

- **移設 (2026-10-27)**: [LUDIARS/ErgoDemo](https://github.com/LUDIARS/ErgoDemo) の `demos/shadow_play/` (target `ergodemo_shadow_play`) へ移した。Pictor ではビルドしない。以下は移設時点の記述で、Ergo でのパラメータ調整は ErgoDemo 側の文書を正とする。

- 依存: `pictor`、Vulkan+GLFW、glslc(無ければtargetを作らない)、stb。
- ライブラリ機能: PC-FEAT-PLAT-01
- demo専用: PCFなしのハードシャドウとセロファン色8灯、障子紙の透過(SSS近似)、切り絵バックドロップ・月光カットアウト・ゴッドレイ(`ErgoDemo:spec/feature/shadow-play-kirie-backdrop.md`)。
- 固有パイプライン: VulkanContextとGLFWだけを使い、影map・紙スクリーン・ゴッドレイのパスをdemo内で構成。ライブラリの影(PC-FEAT-DRAW-03)は使わない。

### PC-DEMO-12 文字描画 — `pictor_text_demo`

- 依存: `pictor`、Vulkan+GLFW。ライブラリ機能: PC-FEAT-PLAT-01、PC-FEAT-DRAW-10
- 固有パイプライン: ライブラリのフォント・ラスタライズ・画像/SVG出力をdemoのoverlay shaderで表示。

### PC-DEMO-13 ポストエフェクト — `pictor_postprocess_demo`

- **移設 (2026-10-27)**: [LUDIARS/ErgoDemo](https://github.com/LUDIARS/ErgoDemo) の `demos/postprocess/` (target `ergodemo_postprocess`) へ移した。Pictor ではビルドしない。以下は移設時点の記述で、Ergo でのパラメータ調整は ErgoDemo 側の文書を正とする。

- 依存: `Pictor::postprocess`、Vulkan+GLFW、`PICTOR_ENABLE_POSTPROCESS=ON`(既定ON。外すとtargetなし)。
- ライブラリ機能: PC-FEAT-PLAT-01、PC-FEAT-DOD-01、PC-FEAT-DOD-08、PC-FEAT-DRAW-08
- 固有パイプライン: demoのHDRシーンshaderに、Bloom・DoF・Tone Mapping・Gaussian Blurをキーで切り替えて適用。

### PC-DEMO-14 VR/OpenXR — `pictor_vr_openxr_demo`

- 依存: `Pictor::xr`、`PICTOR_ENABLE_XR=ON` かつ `PICTOR_ENABLE_OPENXR=ON`(既定OFF)、OpenXR loader、Vulkan。実行にはOpenXRランタイムとHMDが必要。
- ライブラリ機能: PC-FEAT-PLAT-01。XRモジュール(stereo camera・reprojection・delta render)は機能別対応表に機能IDが無い(未登録)。
- demo専用: VRシーン描画・eye target・multiview shader。
- macOSはOpenXR+Vulkan経路を確認していないため対象外とした。

### PC-DEMO-15 Rive再生 — `pictor_rive_demo`

- 依存: `pictor`、Vulkan+GLFW。targetは常に作るが、`PICTOR_ENABLE_RIVE=OFF`(既定)では起動直後に終了する。
- ライブラリ機能: PC-FEAT-PLAT-01、PC-FEAT-DRAW-13
- 固有パイプライン: 公式 `.riv` を `pictor::RiveRenderer` で再生。描画機能はrive-runtime prebuiltが揃った場合だけ。

### PC-DEMO-16 2Dテクスチャ — `pictor_texture2d_demo`

- **移設 (2026-10-27)**: [LUDIARS/ErgoDemo](https://github.com/LUDIARS/ErgoDemo) の `demos/texture2d/` (target `ergodemo_texture2d`) へ移した。Pictor ではビルドしない。以下は移設時点の記述で、Ergo でのパラメータ調整は ErgoDemo 側の文書を正とする。

- 依存: `pictor`、Vulkan+GLFW、stb。ライブラリ機能: PC-FEAT-PLAT-01、PC-FEAT-DOD-08
- demo専用: 2D quad描画(拡縮・回転・不透明度・filter・tint、`texture2d_renderer.cpp`)。
- ライブラリのTextureRegistry(PC-FEAT-DRAW-17)は使わない。

### PC-DEMO-17 SHaRC拡張 Vulkan — `pictor_sharc_demo`

- 依存: `Pictor::sharc_vulkan`(`PICTOR_ENABLE_SHARC` 既定ON、Vulkan検出時に作られる)、Vulkan+GLFW、stb。資産は `tools/download_sharc_demo_assets.py` で別途取得。
- ライブラリ機能: PC-FEAT-PLAT-01、PC-FEAT-DRAW-07
- demo専用: D1 roughness ladder、D2 逆光透過、Bistroクレイレンダ。G-buffer・present・bloom・mesh BVH・PLY/OBJ読込。
- 固有パイプライン: SharcGpuExecutorにdemo独自のG-buffer・BVH・bloom・presentを接続。bloomはライブラリのpostprocessとは別実装。Metal SHaRCは未接続。

### PC-DEMO-18 SHaRC D3D12 headless — `pictor_sharc_dx12_test`

- 依存: `Pictor::sharc_dx12`(Windowsのみ、`PICTOR_ENABLE_SHARC_DX12` はWIN32で既定ON)。Vulkan/GLFW非依存。HLSLはD3DCompileで実行時コンパイル。
- ライブラリ機能: PC-FEAT-DRAW-07
- 固有パイプライン: ウィンドウなし。Bistro OBJを固定カメラで60フレーム実行し `sharc_dx12_out.ppm` を出力。メッシュ読込はPC-DEMO-17とソースを共有するが、パイプラインは別。一般DX12描画backend(PC-FEAT-PLAT-06)とは独立。

### PC-DEMO-19 関係グラフ・dock — `pictor_graph_demo`

- 依存: `pictor`、Vulkan+GLFW。ライブラリ機能: PC-FEAT-PLAT-01
- demo専用: インスタンス描画のグラフview、可視subsetカリング、spatial grid hit-test、dock shell、合成/clangdデータチャネル。
- 固有パイプライン: demo独自のnode/edge/UI矩形shader。ライブラリのUI(PC-FEAT-DRAW-12)やculling(PC-FEAT-DOD-04)は使わない。Iterの表示方式を検証する題材だが、Iterのコードへは依存しない。

### PC-DEMO-20 WebGL2 — `pictor_webgl_demo`

- 依存: `pictor_webgl`、Emscriptenのみ。ライブラリ機能: PC-FEAT-PLAT-02、PC-FEAT-DOD-01
- 固有パイプライン: 独立した最小WebGLRendererをrequestAnimationFrameで駆動。native coreのVulkan効果は含まない。

### PC-DEMO-21 Androidデモ集APK — `pictor_demos`

- 依存: `pictor`、`android`、`log`。`demo/android/CMakeLists.txt` はAndroid NDK専用の別プロジェクトで、SHaRC・postprocess・decal・Riveを強制OFFにしてPictor本体を取り込む。
- ライブラリ機能: PC-FEAT-PLAT-01、PC-FEAT-PLAT-03、PC-FEAT-DOD-01
- 固有パイプライン: Instanced spheres(SimpleRenderer)とSurface recovery(色が変わるrender pass)がそれぞれ別のDemoPipeline実装を持つ。共通化はAndroidライフサイクルとフレーム提出だけで、描画パイプラインは統合しない(`spec/feature/android-demo-collection.md`)。

### PC-DEMO-22 Riveキューブ — targetなし

- `demo/rive_cube/` はどのCMakeからも参照されておらず、ビルド対象ではない。
- ライブラリ機能(ソース上): PC-FEAT-PLAT-01、PC-FEAT-DRAW-13
- demo専用: 6つのRiveRendererを1024pxオフスクリーンへ描き、回転キューブへ貼る。ビルド対象へ戻すかどうかはこの対応表の範囲外。
