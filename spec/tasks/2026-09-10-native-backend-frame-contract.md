---
task: native-backend-frame-contract
project: Pictor
kind: 実装
status: in-progress
created: 2026-09-10
memory_links:
  - spec/feature/dx12-backend-design.md
  - spec/feature/metal-backend-design.md
  - spec/feature/portability/mobile.md
---

# ネイティブ描画対応の第1段階 — フレーム結果とライフサイクル

## 目的

KD-MOB-002 の upstream 契約を Pictor 側で引き取り、Android Vulkan、iOS
ネイティブ Metal、Windows DirectX 12 に共通する復旧判断の語彙を用意する。
2026-09-10 のユーザー指示により iOS の完成形は MoltenVK ではなく Metal
直呼びとする。Windows は既存設計の DirectX 12 対応を追加する。

全バックエンド移植を一括 PR にせず、まず既存 Vulkan のエラー境界と
バックエンド非依存の lifecycle を実装する。本 task 単独でモバイル対応や
Metal / DirectX 描画を完成扱いしない。

## 実装内容

- `FrameStatus` / `FrameResult` は OS / GPU API の型を公開しない。
- `VulkanContext::acquire_frame()` / `present_frame()` を追加する。
  既存 acquire/present のシグネチャと自動 resize 再生成は維持する。
- fence wait/reset、acquire、present、swapchain capability/create/query の
  失敗を結果へ伝える。surface/device loss は通常 resize と区別する。
- `SurfaceLost` / `DeviceLost` / `Error` 後の acquire/present は処理を
  再発行しない。host による明示的な teardown / initialize を要求する。
- provider が `None` を返す間は surface 作成・acquire・present・再生成を
  行わない。provider と native window/layer は従来どおり host 所有。
- app activity と surface availability を別々に保持する。surface 消失中の
  pause/suspend/resume を記憶し、surface 復帰だけで ACTIVE にしない。

## 受け入れ条件

- `acquire_frame().has_image()` が true の場合だけ submit できる。
- suboptimal acquire は有効 image と `recreate_requested=true` を返す。
  semaphore を消費するため、その image を submit/present してから復旧する。
- present 結果の image index は常に無効とし、次の submit 許可には使わない。
- device/surface loss を resize 成功や通常 frame skip として扱わない。
- 未初期化 context の acquire/present は Vulkan を呼ばない。
- lost → suspend → regained は SUSPENDED、lost → pause → regained は PAUSED、
  lost → resume → regained は ACTIVE。重複 regained は状態を変更しない。
- host は owner thread のフレーム境界で lifecycle を処理し、
  `frame_work_suppressed()` の間は acquire/submit/present を呼ばない。
  外部 consumer の `vkQueueSubmit` を本 API が自動遮断するものではない。

## 復旧と互換性の制約

自動 resize は既存の破棄後再生成方式を維持する。replacement-before-retire の
swapchain 全体への拡張と host 所有 framebuffer 等の世代管理は後続 task。
loss 後は host の GPU resource を context shutdown より先に破棄し、native
provider を更新して context を再初期化した後で GPU resource を再構築する。
simulation の CPU state は host が保持する。device lost を旧 device の resize
だけで復旧しない。Error も同期状態が不明になり得るため明示再初期化とする。

## 後続の実装単位

1. RHI の device/resource/command/swapchain 境界を Vulkan の実呼び出しへ接続し、
   host 資源の世代切替と surface/device 復旧を実装する。
2. Android の runtime Vulkan target と host shader tools の分離、KD の依存 pin / adapter 接続。
3. iOS ネイティブ Metal の resource・shader・command・drawable 実装と host 接続。
4. Windows DirectX 12 の resource・shader・command・swapchain 実装と backend 選択。
5. 明示許可後のビルド・回帰・Android/iOS 実機検証。

## 検証・配送

`unit_frame_contract_test` で lifecycle のイベント順序、`FrameResult`
の submit 許可、未初期化 context の結果を headless 検査できる。
ユーザー指示に従いビルド、単体・統合・動作・起動テストは実行していない。
surface/device loss の Vulkan 実動作、Metal / DirectX の実装および実機検証も未完了。
この実装単位は commit と PR 作成までとし、マージ・main 更新は行わない。
