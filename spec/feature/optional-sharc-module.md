# SHaRC選択モジュール

PC-RULE-002の最初の移行単位。SHaRCの実装をcoreの`pictor` archiveから外し、
consumerがbackend別targetを選択する。Ergoやゲーム固有の型・assetは要求しない。

## 選択と依存

| 設定 / target | 役割と依存 |
| --- | --- |
| `PICTOR_ENABLE_SHARC` | 全体のbuildスイッチ。既定ON。OFFはcache済みbackend設定より優先する |
| `PICTOR_ENABLE_SHARC_VULKAN` | Vulkan executor。初回既定はVulkan検出結果。明示ONでVulkanなしはconfigureエラー |
| `PICTOR_ENABLE_SHARC_DX12` | DX12 executor。初回既定はWindowsでON。他OSの明示ONはconfigureエラー |
| `Pictor::sharc_contract` | backend非依存の設定・GPU入力layout。header-only、専用資源なし |
| `Pictor::sharc_vulkan` | SHaRC Vulkan実装。core `pictor` とcontractに依存。coreからの逆依存なし |
| `Pictor::sharc_dx12` | SHaRC DX12実装。contract、d3d12、dxgi、d3dcompilerのみ。core/Vulkan/GLFWへリンクしない |
| `pictor_sharc_shaders` | Vulkan用SPIR-V生成。Vulkanモジュールのみから依存し、demoなしでも利用可能 |

`PICTOR_ENABLE_SHARC=OFF`ならSHaRCの実装target、shader生成、専用demo、
SHaRC専用の既存unit test targetを作らない。共通の`pictor_shaders`にもSHaRCのsource/include依存を残さない。
既存ビルドディレクトリの古いarchive/shaderファイルを自動削除する操作は行わない。
core全体の既存Vulkan/GLFW探索まで無効になるという意味ではない。

## consumer移行

従来の`target_link_libraries(app PRIVATE pictor)`だけではSHaRC実装はリンクされない。
使用するconsumerは次のどちらかを追加する（コマンドの実行結果ではなく設定例）。

```cmake
target_link_libraries(app PRIVATE Pictor::sharc_vulkan)
# DX12 consumerの場合: target_link_libraries(app PRIVATE Pictor::sharc_dx12)
```

対応targetをリンクせずexecutor headerをincludeした場合、target名付きのcompileエラーにする。
手動ビルドでは該当sourceとbackend依存をリンクし、`PICTOR_HAS_SHARC_VULKAN=1`または
`PICTOR_HAS_SHARC_DX12=1`を定義する。マクロだけで実装の欠落を代替しない。
共通値型は`pictor/gi/sharc_config.h`に置き、DX12 headerはVulkan executorをincludeしない。

Vulkan用shaderは`PICTOR_SHARC_SPIRV_DIR`へ生成する。glslcがない場合はその旨をconfigure時に表示し、
consumerが事前生成済みshaderのパスを`initialize()`へ渡す。shader欠落は初期化失敗であり、
no-opの描画へ置き換えない。DX12は`shaders/sharc/hlsl`のHLSLとhlsli一式を配布し、
実行時compilerへディレクトリを渡す。ライブラリ本体にdemoのcwdや開発機のassetパスを埋め込まない。

## 実行時の所有

SHaRCは既存のhost-driven executorであり、自動登録や常駐workerは追加しない。
利用しないhostはexecutorを初期化しない。初期化済みの経路を取り外す場合、hostはGPU使用完了を保証し、
executorの`shutdown()`または破棄を呼ぶ。専用資源の解放はexecutor所有者が担う。
Vulkanの借用contextはexecutorより長生きさせる。DX12版の自己所有device契約は維持する。
実行中の汎用pipeline hot-toggle機構を新設した変更ではない。

## 残作業

他のGI/影/AO、postprocess、文字/UI、vector/Rive、animationのモジュール分割は別単位。
各demoは固有のパイプライン設計を持ち、それぞれを選択可能なデフォルトとして提供する。
demo同士のパイプライン共有・統合は行わない。この個別デフォルトの整備、SHaRCのMetal実装、
共通coreのbackend/windowing分離は未完了。native対応もモジュール単位で進め、全機能を一体化しない。
VulkanとDX12の描画機能が完全に同等であるとは宣言しない。
