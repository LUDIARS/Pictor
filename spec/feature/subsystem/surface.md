# Surface / Swapchain — プラットフォーム抽象 + Vulkan コンテキスト

> 実装: `include/pictor/surface/`, `src/surface/`

ウィンドウシステムと Vulkan の境界。`ISurfaceProvider` でプラットフォームを抽象化し、
`VulkanContext` が instance/device/swapchain のライフタイムを管理する。

## ISurfaceProvider (surface_provider.h)

VulkanContext をウィンドウ実装から切り離すインターフェース。仮想メソッド:

| メソッド | 役割 |
|---|---|
| `NativeWindowHandle get_native_handle() const` | プラットフォーム固有ハンドル (Win32/Xlib/Xcb/Wayland/Cocoa/Android/iOS) |
| `SwapchainConfig get_swapchain_config() const` | 希望 width/height/vsync/image_count (既定 3) |
| `void on_swapchain_created(uint32_t w, uint32_t h)` | swapchain 確定時の通知 |
| `void poll_events()` | 毎フレーム呼ぶ (glfwPollEvents 等)。組込 provider は no-op |
| `bool should_close() const` | 終了要求 (既定 false) |
| `uint32_t get_required_instance_extensions(const char** out, uint32_t max) const` | 必要な VK instance 拡張 (VK_KHR_surface + 各プラットフォーム拡張) |

**2 つの使い方**: ① ホストがウィンドウを所有して `ISurfaceProvider` を実装し VulkanContext へ渡す / ② Pictor 同梱の `GlfwSurfaceProvider` を使う。

### プラットフォーム実装

- **`GlfwSurfaceProvider`** — `GLFWwindow` 所有。`create(GlfwWindowConfig)` / `destroy()`、framebuffer size callback でリサイズ捕捉、`glfwGetRequiredInstanceExtensions` 経由で拡張返却。
- **`AndroidSurfaceProvider`** — `ANativeWindow` 橋渡し。`update_window(ANativeWindow*, w, h)` (onNativeWindowCreated/Destroyed)。window=nullptr で background。拡張 = VK_KHR_surface + VK_KHR_android_surface。常時 vsync。
- **`IOSSurfaceProvider`** — `CAMetalLayer` 橋渡し (MoltenVK)。`update_layer(void*, w, h)` (回転/SafeArea)。拡張 = VK_KHR_surface + VK_EXT_metal_surface。常時 vsync。

## VulkanContext (vulkan_context.h)

VkInstance / VkPhysicalDevice / VkDevice / VkSurfaceKHR / VkSwapchainKHR を所有 (provider は borrow)。

**初期化順 `initialize(ISurfaceProvider*, VulkanContextConfig)`**:

```
create_instance        ← provider の拡張 + 任意で validation layer
create_surface         ← vkCreate*SurfaceKHR (プラットフォーム別)
pick_physical_device   ← graphics + present 対応の最初の GPU
create_logical_device  ← 任意 feature を probe して有効化
                          (tessellation / fillModeNonSolid / samplerAnisotropy …)
                          Rive 用拡張 (VK_EXT_fragment_shader_interlock /
                          rasterization_order_attachment_access) も probe
                          VK_KHR_swapchain 常時有効
create_swapchain       ← format=B8G8R8A8_SRGB 優先、present=FIFO(vsync)/IMMEDIATE/MAILBOX
create_image_views
create_render_pass     ← config.create_default_render_pass=true のとき (color-only, finalLayout=PRESENT_SRC)
create_framebuffers    ← 同上
create_command_pool_and_buffers
create_sync_objects    ← per-frame: image_available_sem / render_finished_sem / in_flight_fence
```

**毎フレーム acquire/present**:

```cpp
uint32_t acquire_next_image();  // waitFence→resetFence→vkAcquireNextImageKHR
                                // VK_ERROR_OUT_OF_DATE → recreate_swapchain() → UINT32_MAX
bool     present(uint32_t image_index);  // vkQueuePresentKHR
                                // OUT_OF_DATE / SUBOPTIMAL → recreate_swapchain()
```

**リサイズ**: `recreate_swapchain()` = deviceWaitIdle → cleanup_swapchain → swapchain/views/(renderpass/FB) 再生成。acquire/present の out-of-date で自動発火、手動でも呼べる。

**`create_default_render_pass=false`**: ホストが profile 駆動の `RenderPassRegistry` / `FramebufferRegistry` で pass/FB を管理する場合 (KuzuSurvivors Phase 4 等)、既定 RP/FB をスキップできる。

**拡張フラグ**: `has_fragment_shader_interlock()` / `has_rasterization_order_attachment_access()` を Rive レンダラの coverage モード選択 (pixel-interlock > ROV > atomic) に渡す。

## SimpleRenderer (simple_renderer.h)

デモ/ベンチ用の最小インスタンスドレンダラ (icosphere インスタンシング)。`initialize(VulkanContext&, shader_dir)` で `simple_inst.{vert,frag}.spv` をロード、`update_instances(data, count)` で SSBO 更新、`render(cmd, render_pass, fb, extent, view, proj)`。プロダクション用ではない (本番は `PictorRenderer`)。

## 起動配線 (demo/main.cpp)

```
GlfwSurfaceProvider::create(config)
VulkanContext::initialize(&provider, cfg)
PictorRenderer::initialize(cfg)  // または SimpleRenderer::initialize(vk, shader_dir)
loop: provider.poll_events() → img=vk.acquire_next_image()
      → (img!=UINT32_MAX) record → vk.present(img)
shutdown: vk.shutdown() → provider.destroy()
```

## 依存

`<vulkan/vulkan.h>`、GlfwSurfaceProvider のみ `<GLFW/glfw3.h>` + `glfw3native.h`。Android は `<android/native_window.h>`、iOS は CAMetalLayer (Obj-C bridge)。volk/VMA は不使用。
