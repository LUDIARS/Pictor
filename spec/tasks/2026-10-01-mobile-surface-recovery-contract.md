---
task: mobile-surface-recovery-contract
project: Pictor
kind: 実装
created: 2026-10-01
memory_links:
  - spec/feature/portability/mobile-surface-recovery.md
  - spec/feature/portability/mobile.md
  - spec/tasks/2026-09-10-native-backend-frame-contract.md
---
# 型付き mobile surface / device recovery 契約の残り

## 目的

第1段階 (`2026-09-10-native-backend-frame-contract`) で入った `FrameStatus` /
`FrameResult` と lifecycle の surface 分離を、host が分岐できる上流契約として
完成させる。surface 消失・device 消失・拡張欠落・lifecycle 抑止を、通常の resize や
frame skip と区別できる型で返し、surface 復帰時の再構築順を公開仕様に固定する。

着手時点の充足状況:

| 条件 | 着手時点 |
|---|---|
| acquire / present 結果が Ready / RecreateSwapchain / SurfaceLost / DeviceLost を区別 | 充足 |
| native window 消失中は surface / swapchain を作らない | 部分 (frame 系のみ。`initialize()` は理由なしの false) |
| MoltenVK portability extension / feature の capability 検査 | 未 |
| required extension 欠落を resize / frame skip へ畳まない | 部分 (bool と stderr のみ) |
| pause / suspend / surface lost 中の GPU submission 抑止 | 部分 (`PictorRenderer` のみ。`VulkanContext` は lifecycle を知らない) |
| surface 復帰時の swapchain 依存 resource 再構築順を spec へ記録 | 未 |
| Android / iOS provider は host 所有のまま | 充足 (変更しない) |
| desktop surface path を同じ typed result へ移行 | 部分 (legacy API が本体、最小化が sticky `Error`) |

## 完了条件

- C-1 gate_frame(input): 未初期化は NotInitialized、latched な SurfaceLost / DeviceLost / Error はそのまま、presentation 停止中は Suspended、native surface 不在は SurfaceLost を返し、いずれも native 呼び出しを許可しない
- C-2 VulkanContext::initialize(provider): native handle が None なら instance / surface / swapchain を作らず `ContextInitStatus::SurfaceUnavailable` を残す
- C-3 first_missing_extension(required, available): 欠落した拡張名を返し、`initialize()` は `MissingInstanceExtension` / `MissingDeviceExtension` として報告する (resize / frame skip に畳まない)
- C-4 first_missing_portability_feature(caps, required): portability subset device で未対応の要求 feature 名を返し、非 subset device では常に nullptr。`initialize()` は `MissingCapability` として報告し、subset device では `VK_KHR_portability_subset` を必ず有効化する
- C-5 VulkanContext::acquire_frame(): `set_presentation_suspended(true)` の間は Vulkan を呼ばず Suspended を返し、image を渡さない
- C-6 FrameResult::swapchain_recreated: context が swapchain を作り直したフレームだけ true になり、host は swapchain 依存 resource を再構築する
- C-7 VulkanContext::recreate_swapchain(): surface extent が 0 (最小化) なら旧 swapchain を保ったまま RecreateSwapchain を返し、Error に latch しない
- C-8 spec: surface 復帰時の再構築順と provider の host 所有を `spec/feature/portability/mobile-surface-recovery.md` に記録する
- C-9 desktop demo (`demo/main.cpp`) は `acquire_frame()` / `present_frame()` の typed result で分岐する

## スコープ (編集可ディレクトリ)

- `include/pictor/surface/`、`src/surface/`
- `tests/` (headless の契約テスト)
- `demo/main.cpp` (desktop reference loop)
- `spec/feature/portability/`、`spec/tasks/`
- `CMakeLists.txt` (新規ソースの登録のみ)

対象外: 特定タイトル向けの world target / touch / アプリパッケージ、Android / iOS
provider の所有モデル変更、Metal / DirectX 12 backend の追加実装、実機検証。
