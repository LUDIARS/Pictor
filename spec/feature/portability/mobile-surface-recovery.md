# Portability — Mobile surface / device recovery 契約

surface を持つ描画 context (`VulkanContext`、`Dx12Context`、`MetalContext`) が host へ返す
型付きの結果と、host が surface / device を取り戻すときの順序を定める公開契約。
Android / iOS / desktop で同じ語彙を使う。総括は [../portability.md](../portability.md)、
モバイル全体の現状は [mobile.md](mobile.md) を参照。

実装: `include/pictor/surface/frame_result.h`、`frame_gate.h`、`context_init_result.h`、
`vulkan_capability_check.h`、`vulkan_context.h`。テスト: `tests/unit_frame_contract_test.cpp`、
`tests/unit_surface_recovery_contract_test.cpp`。

---

## 1. 所有 (ownership)

- native window / layer (`ANativeWindow*`、`CAMetalLayer*`、HWND など) と
  `ISurfaceProvider` 実装 (`AndroidSurfaceProvider`、`IOSSurfaceProvider`、
  `GlfwSurfaceProvider`) は **host が所有する**。context はそれを借用するだけで、
  解放・差し替え・寿命延長をしない。
- context が所有するのは GPU 側のオブジェクト (Vulkan なら instance / surface /
  device / swapchain / 既定 render pass / framebuffers / per-image command buffer /
  同期オブジェクト) だけ。
- host が作った GPU resource (pipeline、host 所有の framebuffer、uniform ring など) は
  host が所有し、破棄と再構築も host が行う。

## 2. フレーム結果 (`FrameResult`)

| `FrameStatus` | 意味 | host がすること |
|---|---|---|
| `Ready` | acquire が image を渡した (`has_image()` が true) | 記録 → submit → present |
| `RecreateSwapchain` | resize / suboptimal / 最小化。通常の復旧 | このフレームは描かない。context が swapchain を自動で作り直す |
| `Suspended` | host が presentation を止めている | 何もしない (native 呼び出しは発生していない) |
| `SurfaceLost` | native surface が消えた | 明示的に teardown → surface 復帰後に再初期化 (§4) |
| `DeviceLost` | GPU device が失われた | 明示的に teardown → 再初期化 (§4) |
| `Error` | 同期状態が不明な失敗 | `DeviceLost` と同じく再初期化 |
| `NotInitialized` | 未初期化 context | 初期化する |

- **submit してよいのは `has_image()` が true のときだけ。** present の結果は image を
  渡さない。
- suboptimal acquire は `Ready` かつ `recreate_requested=true`。semaphore を消費済みなので、
  その image を submit / present してから復旧する。
- `swapchain_recreated=true` は「この呼び出しの中で context が swapchain を作り直した」こと。
  context 所有の既定 render pass / framebuffers は更新済み。host 所有の swapchain 依存
  resource (§5) はここで作り直す。
- `SurfaceLost` / `DeviceLost` / `Error` は **sticky**。以後の acquire / present /
  recreate は native を呼ばずに同じ値を返す (`frame_status_requires_reinitialize()`)。
  これらを resize や通常の frame skip として扱ってはならない。
- 最小化などで surface の面積が 0 になったときは `RecreateSwapchain` のまま旧 swapchain を
  保持する。`Error` には落とさない。

判定順 (`gate_frame()`): `NotInitialized` → sticky な loss / error → `Suspended` →
native window 不在 (`SurfaceLost`) → native 呼び出しへ進む。

## 3. 初期化結果 (`ContextInitResult`)

`initialize()` の bool に加えて `init_result()` が理由を返す。失敗した初期化の
teardown 後も値は残る。

| `ContextInitStatus` | 例 |
|---|---|
| `SurfaceUnavailable` | provider が native window を持たない / surface の面積が 0。**native は何も作っていない** |
| `MissingInstanceExtension` | `VK_KHR_android_surface`、`VK_EXT_metal_surface` など provider / 外部要求の拡張が loader に無い |
| `MissingDeviceExtension` | `VK_KHR_swapchain`、外部要求のデバイス拡張が無い |
| `MissingCapability` | portability subset で要求 feature が無い、multiview、swapchain usage、surface format 0 件 |
| `NoSuitableDevice` | GPU が無い / graphics + present の queue が無い |
| `Failed` | 上記以外 (step 名を `detail` に残す) |

拡張・capability の欠落は resize や frame skip に畳まない。host は status で分岐し、
`SurfaceUnavailable` だけを「surface が戻ったら再試行」として扱う。

### portability (MoltenVK など)

- loader が `VK_KHR_portability_enumeration` を持つときは有効にし、
  `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` を立てる (無いと portability
  実装の GPU が列挙されない)。
- 選んだ device が `VK_KHR_portability_subset` を公開していれば **必ず有効化する**
  (Vulkan の要件)。その feature bit を `vkGetPhysicalDeviceFeatures2` で読み、
  `VulkanContext::portability()` で公開する。
- host は依存する subset feature を `VulkanContextConfig::required_portability_features`
  で宣言する。subset device が満たさなければ `MissingCapability` で初期化を失敗させる。
  非 subset (conformant) device では要求は常に満たされる。

## 4. lifecycle と GPU submission の抑止

- app の活動状態と surface の有無は `MobileLifecycleController` が別々に持つ
  (`on_pause` / `on_resume` / `on_suspend` / `on_surface_lost` / `on_surface_regained`)。
- host は owner thread のフレーム境界で
  `context.set_presentation_suspended(controller.frame_work_suppressed())` を呼ぶ。
  停止中の acquire / present / recreate は Vulkan を呼ばず `Suspended` を返すため、
  その上に submit を組み立てられない。`PictorRenderer` の `begin_frame` / `render` /
  `end_frame` も同じ条件で no-op になる。
- `Suspended` は loss ではない。presentation を止めてから native window を手放せば、
  context は `SurfaceLost` を latch しない。
- 外部 consumer が自前で発行する `vkQueueSubmit` までは context が遮断しない。
  host は `has_image()` が false のフレームで submit しない。

## 5. surface 復帰時の再構築順

### 5.1 resize / 最小化からの復帰 (`RecreateSwapchain`)

context が自動で行う。旧 swapchain の一式は代替が使えるまで保持される
(`oldSwapchain` 経由の replacement-before-retire)。

1. device idle を待つ
2. swapchain を作り直す (面積 0 なら旧一式を保持して `RecreateSwapchain` のまま終える)
3. swapchain image view
4. 既定 render pass → 既定 framebuffers (`create_default_render_pass=true` のとき)
5. per-image resource (command buffer / render-finished semaphore / images-in-flight)
6. 旧 framebuffers → 旧 render pass → 旧 image view → 旧 swapchain の順に破棄
7. `swapchain_recreated=true` を返す → host は自分の swapchain 依存 resource
   (host 所有の framebuffer、extent 依存の attachment / viewport、image 数依存の
   per-image データ) を作り直す

### 5.2 surface 消失からの復帰 (Android `onNativeWindowDestroyed` → `Created`、iOS layer 差し替え)

1. **消失時**: `on_surface_lost()` → `set_presentation_suspended(true)`。
   以後 acquire / submit / present をしない。
2. host が GPU 側の処理完了を待つ (`device_wait_idle()`)。
3. host 所有の GPU resource を破棄する (context より先)。
4. `context.shutdown()`。native window は host が解放する
   (provider には `update_window(nullptr, ...)` / `update_layer(nullptr, ...)`)。
5. **復帰時**: host が新しい native window を provider へ渡す
   (`update_window(window, w, h)` / `update_layer(layer, w, h)`)。
6. `context.initialize(provider)`。`init_result()` が `SurfaceUnavailable` なら
   window がまだ無いので 5 から待つ。他の失敗は capability の問題として扱う。
7. host 所有の GPU resource を再構築する (swapchain 依存 → それ以外の順で、
   swapchain の format / extent / image 数を context から読み直す)。
8. `on_surface_regained()` → `set_presentation_suspended(frame_work_suppressed())`。
   app が pause / suspend 中なら抑止は続く。

simulation などの CPU 側 state は host が保持し、作り直さない。

### 5.3 device 消失 / Error からの復帰

5.2 の 2〜7 と同じ順序で行う (native window は保持したまま)。旧 device の swapchain を
作り直すだけでは復旧しない。

## 6. desktop

desktop (GLFW) も同じ `acquire_frame()` / `present_frame()` を使う。`demo/main.cpp` が
参照ループ。最小化は `RecreateSwapchain` が続き、復帰後に自動で作り直される。
legacy の `acquire_next_image()` / `present()` は typed API の薄い wrapper として残り、
image を渡さない全ての結果で `UINT32_MAX` / false を返す。

## 7. 未検証

- Android / iOS 実機、MoltenVK 実機での portability 経路は未検証 (headless テストは
  判定ロジックと未初期化 / surface 不在の経路のみ)。
- `Dx12Context` / `MetalContext` は §2 の語彙を返すが、`Suspended` と
  `ContextInitResult` はまだ持たない。
