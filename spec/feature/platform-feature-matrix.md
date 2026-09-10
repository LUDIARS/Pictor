# Pictor 機能別プラットフォーム対応表

更新: 2026-09-10。対応の目標ではなく、参照したソースに存在する実装経路を示す。
この表の「実装あり」はビルド成功・実機動作・性能保証を意味しない。

## 判定凡例

- CPU: 共通C++側の実装あり。各OSのコンパイル・asset I/O・thread・host接続は未検証。GPU機能の移植済みとは扱わない。
- VK / DX12 / Web実装: 該当API向けの実装またはshaderが存在する。完全性は行ごとの注記に従う。
- 部分: stub、未接続処理、hostによる描画記録などの残作業あり。
- 未接続: このsnapshotで対応実装・接続を確認していない。技術的に実現不能という意味ではない。
- 別枝: native実装ブランチ47967d8で確認。本体mainやリリースへの反映済みとは断定しない。
- ※ macOS/Vulkanは既存CMakeとGLFWの構成経路。Vulkan提供環境が別途必要で、ネイティブMetalとは別。
- † Androidは下記クロスビルド失敗により全体ビルド未成立。VK表記はソース共有経路の存在だけ。
- ‡ iOSはSDKビルド・実機確認なし。CPUソースの存在だけでiOS製品対応済みとはしない。

## 検証状態

| 対象 | 実施結果 |
|---|---|
| Android ARM64 / API 26 / NDK 21.4.7075529 / Debug | CMake configure成功。pictorコンパイルはpool_allocator.cpp:103のstd::aligned_alloc未提供で停止 |
| Androidの他NDK・API level・ABI | 未実施。API 26/NDK r21での失敗をAndroid全環境の非対応に一般化しない |
| Android実機・背景復帰・surface/device復旧 | 未確認 |
| iOS/Metal | ビルド・実機とも未確認 |
| Windows/Linux/macOS/Webの各機能 | この表作成ではビルド・実行を追加していない。過去PRのTest OKを各機能の実機確認へ転用しない |

Android確認対象は47967d8、CMake 4.1.2、-DPICTOR_BUILD_DEMO=OFF -DPICTOR_BUILD_TESTS=OFF -DPICTOR_BUILD_TOOLS=OFF。テストやアプリ起動は行っていない。

## 機能一覧

| ID / 機能 | Web/WebGL2 | Windows/Vulkan | Windows/DX12 | Linux/Vulkan | macOS/Vulkan | Android/Vulkan | iOS/Metal |
|---|---|---|---|---|---|---|---|
| PC-FEAT-DOD-01 ObjectDescriptor・SoA描画オブジェクト管理 | 未接続 | CPU | CPU | CPU | CPU | CPU† | CPU‡ |
| PC-FEAT-DOD-02 フレーム・プール・GPU論理メモリ管理 | 未接続 | CPU | CPU | CPU | CPU | 失敗† | CPU‡ |
| PC-FEAT-DOD-03 radix sortと描画バッチ構築 | 未接続 | CPU | CPU | CPU | CPU | CPU† | CPU‡ |
| PC-FEAT-DOD-04 フラスタム・flat BVHカリング | 未接続 | CPU | CPU | CPU | CPU | CPU† | CPU‡ |
| PC-FEAT-DOD-05 ワールド空間分割 | 未接続 | CPU | CPU | CPU | CPU | CPU† | CPU‡ |
| PC-FEAT-DOD-06 ジョブ分配と更新スケジューリング | 要thread構成 | CPU | CPU | CPU | CPU | CPU† | CPU‡ |
| PC-FEAT-DOD-07 GPU-driven update/cull/LOD/indirect draw | 未接続 | 部分VK | 未接続 | 部分VK | 部分VK※ | 部分VK† | 未接続 |
| PC-FEAT-DOD-08 CPU/GPU性能計測・統計照会・出力 | 未接続 | CPU+VK | CPUのみ | CPU+VK | CPU+VK※ | CPU+VK† | CPUのみ‡ |
| PC-FEAT-DOD-09 UMA/ReBARメモリ判定 | 未接続 | 部分VK | 未接続 | 部分VK | 部分VK※ | 部分VK† | 未接続 |
| PC-FEAT-PLAT-01 PC Vulkanデバイス・surface・swapchain | 対象外 | VK | 対象外 | VK | VK※ | 別provider† | 対象外 |
| PC-FEAT-PLAT-02 WebGL2/Emscripten描画 | Web実装 | 対象外 | 対象外 | 対象外 | 対象外 | 対象外 | 対象外 |
| PC-FEAT-PLAT-03 Android native surfaceとNDKビルド分岐 | 対象外 | 対象外 | 対象外 | 対象外 | 対象外 | 部分VK† | 対象外 |
| PC-FEAT-PLAT-04 モバイルpause/suspend・memory/thermal制御 | 未接続 | 対象外 | 対象外 | 対象外 | 対象外 | 部分CPU† | 部分CPU‡ |
| PC-FEAT-PLAT-05 iOSネイティブMetal描画 | 対象外 | 対象外 | 対象外 | 対象外 | 対象外 | 対象外 | Metal別枝‡ |
| PC-FEAT-PLAT-06 Windows DirectX 12描画バックエンド | 対象外 | 対象外 | DX12別枝 | 対象外 | 対象外 | 対象外 | 対象外 |
| PC-FEAT-PLAT-07 C ABI組み込み | 未接続 | 部分CPU | 要配線 | 部分CPU | 部分CPU | 要配線† | 要配線‡ |
| PC-FEAT-PIPE-01 描画プロファイル・JSON設定・プリセット | 未接続 | CPU | CPU | CPU | CPU | CPU† | CPU‡ |
| PC-FEAT-PIPE-02 attachment/render pass/framebuffer構成 | 未接続 | VK | 未接続 | VK | VK※ | VK† | 未接続 |
| PC-FEAT-PIPE-03 コンパイル済みパスとbatch記録 | 未接続 | 部分VK | 未接続 | 部分VK | 部分VK※ | 部分VK† | 未接続 |
| PC-FEAT-PIPE-04 パイプライン・シェーダのホットリロード | 未接続 | CPU/host | CPU/host | CPU/host | CPU/host | 要host† | 要host‡ |
| PC-FEAT-VISUS-01 Visus描画定義・メタデータ・保存 | 未接続 | CPU | CPU | CPU | CPU | CPU† | CPU‡ |
| PC-FEAT-VISUS-02 Visusカタログ・runtime解決・実体化 | 未接続 | CPU | CPU | CPU | CPU | CPU† | CPU‡ |
| PC-FEAT-VISUS-03 Visus shader packageと版移行 | 未接続 | CPU | CPU | CPU | CPU | CPU† | CPU‡ |
| PC-FEAT-DRAW-01 マテリアル定義・保存・カスタムシェーダ | 未接続 | CPU | CPU | CPU | CPU | CPU† | CPU‡ |
| PC-FEAT-DRAW-02 PBR・Lit・Toon・ホログラム | 未接続 | VK | 未接続 | VK | VK※ | VK† | 未接続 |
| PC-FEAT-DRAW-03 影・CSM shadow atlas | 未接続 | 部分VK | 未接続 | 部分VK | 部分VK※ | 部分VK† | 未接続 |
| PC-FEAT-DRAW-04 SSAO | 未接続 | VK | 未接続 | VK | VK※ | VK† | 未接続 |
| PC-FEAT-DRAW-05 GI bake・irradiance probe・relight | 未接続 | CPU+VK | CPUのみ | CPU+VK | CPU+VK※ | CPU+VK† | CPUのみ‡ |
| PC-FEAT-DRAW-06 反射プローブ | 未接続 | 部分VK | 未接続 | 部分VK | 部分VK※ | 部分VK† | 未接続 |
| PC-FEAT-DRAW-07 SHaRCライティングキャッシュ Vulkan/DX12 | 未接続 | VK | DX12 | VK | VK※ | VK† | 未接続 |
| PC-FEAT-DRAW-08 ポストエフェクトチェーン | 未接続 | VK | 未接続 | VK | VK※ | VK† | 未接続 |
| PC-FEAT-DRAW-09 デカール | 未接続 | VK | 未接続 | VK | VK※ | VK† | 未接続 |
| PC-FEAT-DRAW-10 フォント・文字画像・SVG描画 | 未接続 | CPU | CPU | CPU | CPU | CPU† | CPU‡ |
| PC-FEAT-DRAW-11 文字・グリフパスエフェクト | 未接続 | CPU | CPU | CPU | CPU | CPU† | CPU‡ |
| PC-FEAT-DRAW-12 UI・screen overlay | 未接続 | VK | 未接続 | VK | VK※ | VK† | 未接続 |
| PC-FEAT-DRAW-13 Riveベクター描画 | 未接続 | 条件VK | 未接続 | 条件VK | 条件VK※ | 要依存確認† | 未接続 |
| PC-FEAT-DRAW-14 骨格・clip・IK・montage | 未接続 | CPU | CPU | CPU | CPU | CPU† | CPU‡ |
| PC-FEAT-DRAW-15 FBX/BVH取り込み | 未接続 | CPU | CPU | CPU | CPU | CPU† | CPU‡ |
| PC-FEAT-DRAW-16 2D・vector・Lottie・Rive animation | 未接続 | 部分CPU | 部分CPU | 部分CPU | 部分CPU | 部分CPU† | 部分CPU‡ |
| PC-FEAT-DRAW-17 メッシュ・テクスチャ・モデル登録/照会 | 別Web実装 | 部分CPU/VK | 別枝meshのみ | 部分CPU/VK | 部分CPU/VK※ | 部分CPU/VK† | 別枝meshのみ‡ |
| PC-FEAT-TOOLS-01 描画デモ・機能選択・Visus移行ツール | 一部Web | PC demo | SHaRC demo | PC demo | PC demo※ | demo無効 | demo無効 |
| PC-FEAT-HW-01 Intel PCMハードウェアカウンター | 未接続 | 条件CPU | 条件CPU | 条件CPU | 未確認 | 未接続 | 未接続 |

## 機能ごとの制限と根拠

### PC-FEAT-DOD-01 ObjectDescriptor・SoA描画オブジェクト管理

登録・分類・更新・削除。

根拠: `src/scene/soa_stream.cpp`、`src/scene/object_pool.cpp`、`src/scene/object_classifier.cpp`

### PC-FEAT-DOD-02 フレーム・プール・GPU論理メモリ管理

GPU論理割当と実転送の完成を区別する。

根拠: `src/memory/frame_allocator.cpp`、`src/memory/pool_allocator.cpp`、`src/memory/gpu_memory_allocator.cpp`

### PC-FEAT-DOD-03 radix sortと描画バッチ構築

描画状態による並べ替えとbatch生成。

根拠: `src/batch/radix_sort.cpp`、`src/batch/batch_builder.cpp`

### PC-FEAT-DOD-04 フラスタム・flat BVHカリング

CPU可視判定。GPU-driven統合とは別。

根拠: `src/culling/frustum_culler.cpp`、`src/culling/flat_bvh.cpp`

### PC-FEAT-DOD-05 ワールド空間分割

空間領域による管理。

根拠: `src/culling/world_partition.cpp`

### PC-FEAT-DOD-06 ジョブ分配と更新スケジューリング

Webはthread実行条件への対応が別途必要。

根拠: `src/update/job_dispatcher.cpp`、`src/update/update_scheduler.cpp`

### PC-FEAT-DOD-07 GPU-driven update/cull/LOD/indirect draw

シェーダとAPIはあるがGPUDrivenPipeline::executeのcompute dispatchは未実装。

根拠: `src/gpu/gpu_driven_pipeline.cpp`、`shaders/compute_cull.comp`、`shaders/lod_select.comp`

### PC-FEAT-DOD-08 CPU/GPU性能計測・統計照会・出力

timer・統計・出力。HW counterは端末能力と設定に依存。

根拠: `src/profiler/profiler.cpp`、`src/profiler/gpu_timer.cpp`、`src/profiler/perf_query_api.cpp`、`src/profiler/data_exporter.cpp`

### PC-FEAT-DOD-09 UMA/ReBARメモリ判定

判定処理あり。staging省略の実アップロード完成ではない。

根拠: `src/core/device_memory_profile.cpp`、`spec/feature/subsystem/uma_memory.md`

### PC-FEAT-PLAT-01 PC Vulkanデバイス・surface・swapchain

GLFWとper-image同期。完全なsurface/device復旧は後続。

根拠: `src/surface/vulkan_context.cpp`、`src/surface/glfw_surface_provider.cpp`

### PC-FEAT-PLAT-02 WebGL2/Emscripten描画

独立した最小renderer。本体の全描画機能との同等性はない。

根拠: `src/webgl/webgl_renderer.cpp`、`spec/feature/portability/web.md`

### PC-FEAT-PLAT-03 Android native surfaceとNDKビルド分岐

providerとVulkan作成経路。APK・consumer接続・実機検証は未完了。

根拠: `src/surface/android_surface_provider.cpp`、`CMakeLists.txt`

### PC-FEAT-PLAT-04 モバイルpause/suspend・memory/thermal制御

状態制御あり。OS host配線と全GPU submit抑止は未完了。PR1638の改善は解析後にマージ済み（5b98c9bebe7b）。

根拠: `src/core/mobile_lifecycle_controller.cpp`、`include/pictor/core/mobile_lifecycle.h`

### PC-FEAT-PLAT-05 iOSネイティブMetal描画

47967d8のMetalContextにmesh upload/drawと復旧APIあり。PR #1640のマージ状態は未確認。各効果のMetal実装を意味しない。

根拠: `src/surface/ios_surface_provider.cpp`、`spec/feature/metal-backend-design.md`

### PC-FEAT-PLAT-06 Windows DirectX 12描画バックエンド

47967d8のDx12Contextにmesh upload/drawと復旧APIあり。SHaRC DX12 executorとは独立。PR #1640のマージ状態は未確認。

根拠: `src/gi/sharc_dx12_executor.cpp`、`spec/feature/dx12-backend-design.md`

### PC-FEAT-PLAT-07 C ABI組み込み

C APIあり。モバイルsurface/lifecycle入口と全backend同等接続は未完了。

根拠: `include/pictor/c_api.h`、`spec/feature/portability/mobile.md`

### PC-FEAT-PIPE-01 描画プロファイル・JSON設定・プリセット

宣言的設定。実GPUパスへの接続範囲は別途確認する。

根拠: `src/pipeline/pipeline_profile_loader.cpp`、`src/pipeline/pipeline_profile_serializer.cpp`

### PC-FEAT-PIPE-02 attachment/render pass/framebuffer構成

系統B registry/compilerによるVulkan資源構成。

根拠: `src/pipeline/attachment_registry.cpp`、`src/pipeline/render_pass_registry.cpp`、`src/pipeline/framebuffer_registry.cpp`、`src/pipeline/pipeline_compiler.cpp`

### PC-FEAT-PIPE-03 コンパイル済みパスとbatch記録

driver/recorderあり。SHADOW/DEPTH_ONLYの記録は未実装を明示。

根拠: `src/pipeline/compiled_path_driver.cpp`、`src/pipeline/compiled_batch_recorder.cpp`

### PC-FEAT-PIPE-04 パイプライン・シェーダのホットリロード

変更検知とcallback。GPU資源rebuildはhostが配線する。

根拠: `src/core/file_watch.cpp`、`src/pipeline/pipeline_hot_reload.cpp`

### PC-FEAT-VISUS-01 Visus描画定義・メタデータ・保存

DoDとフレキシブルパイプラインの両方に帰属。

根拠: `src/visus/visus_types.cpp`、`src/visus/visus_metadata.cpp`、`src/visus/visus_serializer.cpp`

### PC-FEAT-VISUS-02 Visusカタログ・runtime解決・実体化

描画定義からObjectDescriptorへ接続。

根拠: `src/visus/visus_catalog.cpp`、`src/visus/visus_runtime.cpp`、`src/visus/visus_instantiator.cpp`

### PC-FEAT-VISUS-03 Visus shader packageと版移行

package管理、互換・移行。

根拠: `src/visus/visus_shader_package.cpp`、`src/visus/visus_package_catalog.cpp`、`src/visus/visus_migrate.cpp`

### PC-FEAT-DRAW-01 マテリアル定義・保存・カスタムシェーダ

material builder/serializerとshader registry。

根拠: `src/material/base_material_builder.cpp`、`src/material/material_serializer.cpp`、`src/shader/shader_registry.cpp`

### PC-FEAT-DRAW-02 PBR・Lit・Toon・ホログラム

シェーダ資産あり。全backendへの移植は未完了。

根拠: `shaders/pbr.frag`、`shaders/lit.frag`、`shaders/toon.frag`、`shaders/hologram.frag`

### PC-FEAT-DRAW-03 影・CSM shadow atlas

atlas/深度資源あり。実シーンのshadow描画はhostの接続責務。

根拠: `src/gi/gi_shadow_atlas.cpp`、`shaders/shadow_depth.vert`

### PC-FEAT-DRAW-04 SSAO

compute executorとshader。

根拠: `src/gi/gi_ssao_compute.cpp`、`shaders/ssao_gen.comp`

### PC-FEAT-DRAW-05 GI bake・irradiance probe・relight

CPU bake/probeとGPU sample経路。host接続は別途。

根拠: `src/gi/gi_bake.cpp`、`src/gi/gi_probe_field.cpp`、`src/gi/gi_gpu_executor.cpp`

### PC-FEAT-DRAW-06 反射プローブ

cubemap/mip資源処理。シーンcaptureはhost責務。

根拠: `src/gi/gi_reflection_probe.cpp`

### PC-FEAT-DRAW-07 SHaRCライティングキャッシュ Vulkan/DX12

専用executorとcompute shader。一般DX12描画backendとは別。

根拠: `src/gi/sharc_executor.cpp`、`src/gi/sharc_dx12_executor.cpp`

### PC-FEAT-DRAW-08 ポストエフェクトチェーン

chain/config bridgeとBloom等のshader。

根拠: `src/postprocess/postprocess_pipeline.cpp`、`src/postprocess/postprocess_chain.cpp`、`shaders/postprocess/bloom_extract.frag`

### PC-FEAT-DRAW-09 デカール

管理処理と専用shader。

根拠: `src/decal/decal_system.cpp`、`shaders/decal/decal.frag`

### PC-FEAT-DRAW-10 フォント・文字画像・SVG描画

フォント読込、ラスタライズ、画像/SVG出力。

根拠: `src/text/font_loader.cpp`、`src/text/text_rasterizer.cpp`、`src/text/text_image_renderer.cpp`、`src/text/text_svg_renderer.cpp`

### PC-FEAT-DRAW-11 文字・グリフパスエフェクト

文字とパスの表現処理。

根拠: `src/text/text_effects.cpp`、`src/text/glyph_path_effects.cpp`

### PC-FEAT-DRAW-12 UI・screen overlay

renderer/overlay pipeline/group。

根拠: `src/ui/ui_renderer.cpp`、`src/ui/ui_overlay_pipeline.cpp`、`src/ui/screen_overlay_group.cpp`

### PC-FEAT-DRAW-13 Riveベクター描画

adapterあり。PICTOR_ENABLE_RIVEと外部prebuiltが必要。モバイルは別途。

根拠: `src/vector/rive_renderer.cpp`、`cmake/FindRive.cmake`

### PC-FEAT-DRAW-14 骨格・clip・IK・montage

animation systemと骨格アニメーション。

根拠: `src/animation/skeleton.cpp`、`src/animation/animation_clip.cpp`、`src/animation/ik_solver.cpp`、`src/animation/montage.cpp`

### PC-FEAT-DRAW-15 FBX/BVH取り込み

FBX scene/importerとBVH importer。全形式機能の完全性は未検証。

根拠: `src/animation/fbx_importer.cpp`、`src/animation/fbx_scene.cpp`、`src/animation/bvh_importer.cpp`

### PC-FEAT-DRAW-16 2D・vector・Lottie・Rive animation

adapter/APIあり。形式別の完全性と外部依存は未検証。

根拠: `src/animation/animation_2d.cpp`、`src/animation/vector_animation.cpp`、`src/animation/lottie_animation.cpp`、`src/animation/rive_animation.cpp`

### PC-FEAT-DRAW-17 メッシュ・テクスチャ・モデル登録/照会

registry/queryあり。一部uploadはstaging確保とフラグ更新だけで実GPU転送未完了。

根拠: `src/data/vertex_data_uploader.cpp`、`src/data/texture_registry.cpp`、`src/data/model_data_handler.cpp`、`src/data/data_query_api.cpp`

### PC-FEAT-TOOLS-01 描画デモ・機能選択・Visus移行ツール

demo/tools targetあり。毛皮・縄・涙・影絵等のdemo専用表現を汎用library APIと区別する。

根拠: `CMakeLists.txt`、`spec/tasks/2026-09-02-fbx-viewer-fur-effects.md`、`spec/feature/shadow-play-kirie-backdrop.md`

### PC-FEAT-HW-01 Intel PCMハードウェアカウンター

PCM prebuilt、対応CPU、権限/driverが必要。ARM Android/iOS向けカウンター実装ではない。

根拠: `src/profiler/hardware_counters.cpp`、`CMakeLists.txt`

## 参照snapshotとモジュール分離状態

- 基本機能: ローカルmain 5822f253。Anatomia解析・既存41機能一覧と実ソースを照合。
- Metal/DX12基本描画: feat/native-metal-dx12-recovery / 47967d8。iOS Metalはここにある実装を参照し、古いmainのMoltenVK方針を採用しない。
- SHaRC選択モジュール: PR #1653、ユーザー通知で35f0f3c011bdへマージ済み。Vulkan/DX12個別選択。Metal SHaRCは未接続。
- postprocess/decal分離: PR #1667 / 661a02f、提出済み。対応表作成時にマージ結果は未通知。GPU APIの実装追加ではなくビルド・リンク境界の分離。
- 標準WebGL targetは独立最小renderer。native coreのCPU機能やVulkan効果が自動的にWebへ組み込まれるわけではない。
- 各demoは固有設計の個別デフォルト。モジュールを選んで使い、共通の万能パイプラインやnative全機能統合を完成条件にしない。
- この一覧は登録済みライブラリ機能が中心。毛皮・縄・涙などdemo専用表現はTOOLS-01にまとめ、各demo独自の対応表は別途展開する。
