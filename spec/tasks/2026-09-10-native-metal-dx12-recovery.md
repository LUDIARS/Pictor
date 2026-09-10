---
task: pictor-native-metal-dx12-recovery
project: Pictor
kind: implementation
status: implemented
date: 2026-09-10
---

# Native Metal and DirectX 12 rendering recovery

Pictor provides native presentation contexts for iOS Metal and Windows DirectX
12. Both contexts own their GPU objects while borrowing the host-owned
`CAMetalLayer` or `HWND`, render a triangle pass, and expose acquire and present
through `FrameResult`. `NativeRenderConfig::draw` receives the active native
render encoder/list so consumers can record their full draw workload inside the
same acquired swapchain frame.

`NativeMeshView` is the shared vertex/index input contract. `upload_mesh`
creates real Metal or D3D12 GPU buffers and both paths issue indexed or
non-indexed draws. The contexts retain a CPU copy and recreate those buffers
after device or surface replacement. Backend-specific instancing, material,
texture, and pipeline bindings can be recorded through the draw callback.

Surface replacement creates a complete replacement context before retiring the
old context when the native handle changes. Metal uses that ordering for device
recovery. DX12 device removal invalidates the old device, so recovery releases
it and reconstructs the complete device, queue, swapchain, render-target,
pipeline, command, and fence graph. Same-window resize waits for GPU idle before
`ResizeBuffers`, as required by DXGI.

Android keeps its existing Vulkan path. Vulkan resize now supplies the old
swapchain to `vkCreateSwapchainKHR`, keeps its dependent render graph alive until
the replacement swapchain is created, and retires old dependencies only after
the replacement graph succeeds. If a post-creation dependency fails, both the
new partial graph and Vulkan-retired old graph are closed instead of exposing a
half-valid surface.

Native compilation and runtime validation require Xcode/iOS hardware and a
Windows DX12 runtime. They were not run in this task.

This unit does not migrate every existing `PictorRenderer` Vulkan effect to a
renderer-wide RHI. Metal SHaRC compute and the remaining profile-driven passes
stay in the follow-up phases documented in `metal-backend-design.md` and
`dx12-backend-design.md`.
