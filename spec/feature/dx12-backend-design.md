# DirectX 12 Backend — 設計検討 (RHI 抽象化)

> ステータス: **設計のみ (未実装)**。Pictor は現状 Vulkan 専用。本書は DX12 を
> 併設するための RHI (Render Hardware Interface) 抽象と段階的移行計画を示す。
> キャッシュ整列 / UMA とは独立した別軸 (GPU API の話)。

## 1. 目的とスコープ

PC で **Vulkan に加えて DirectX 12** を選べるようにする。狙い:

- Windows ネイティブ最適化 (WDDM/PIX/GPU ベンダドライバの DX12 経路)。
- Vulkan が弱い/不安定な環境 (一部 Intel/古い AMD ドライバ) の代替。
- 将来 Xbox 系への展開余地。

**非目標**: DoD コア (scene/batch/culling/memory の SoA ロジック) の書き換え。
これらは GPU API 非依存であり、RHI 抽象の下でそのまま再利用する。

## 2. 現状の API 結合度 (どこを抽象化すべきか)

Pictor は幸い**公開 API が既にほぼ抽象**:

- `PictorRenderer` / `c_api.h` / `ObjectDescriptor` 入力 — Vulkan 型を漏らさない。
- `MemorySubsystem` / `SceneRegistry` / `BatchBuilder` / `CullingSystem` /
  `GpuMemoryAllocator` (論理サブアロケータ) — **API 非依存**。これらは無改修で済む。

一方、**Vulkan が露出している箇所** = RHI 化の対象:

| 箇所 | Vulkan 依存 | RHI 後 |
|---|---|---|
| `surface/vulkan_context.{h,cpp}` | VkInstance/Device/Queue/Swapchain | `IDevice` / `ISwapchain` |
| `profiler/gpu_timer.{h,cpp}` | VkQueryPool / vkCmdWriteTimestamp | `IQueryHeap` (timestamp) |
| `pipeline/render_pass_scheduler.cpp` | VkCommandBuffer 記録 | `ICommandEncoder` |
| `profiler/batch_gpu_timer` | VkCommandBuffer | `ICommandEncoder` 経由 |
| `gpu/gpu_buffer_manager` | (現状ほぼ論理。実 VkBuffer は将来) | `IBuffer` |
| `data/{vertex_data_uploader,texture_registry}` | (upload はスタブ) | `IBuffer`/`ITexture` + upload |
| `surface/*_surface_provider` | VkSurfaceKHR (GLFW/Android/iOS) | swapchain 生成は `IDevice` 委譲 |
| shaders/*.{vert,frag,comp} | GLSL→SPIR-V | HLSL→(SPIR-V+DXIL) |

`#ifdef PICTOR_HAS_VULKAN` ガードが既に分離線になっており、ここを
`PICTOR_RHI_VULKAN` / `PICTOR_RHI_D3D12` に再編するのが自然。

## 3. 提案 RHI インターフェース (Interface Segregation)

太い 1 個ではなく責務別に薄く切る (Pictor の `ICuller`/`IShader`/`IPass` と同方針):

```text
IRhiDevice        — 生成/破棄、capability 照会、queue 取得、UMA 判定
  ├ create_buffer(BufferDesc)        -> IRhiBuffer
  ├ create_texture(TextureDesc)      -> IRhiTexture
  ├ create_pipeline(PipelineDesc)    -> IRhiPipeline
  ├ create_query_heap(kind, count)   -> IRhiQueryHeap
  ├ create_swapchain(SurfaceDesc)    -> IRhiSwapchain
  └ memory_profile()                 -> DeviceMemoryProfile   (Task 2 と合流)

IRhiCommandEncoder — フレームのコマンド記録 (VkCommandBuffer / ID3D12GraphicsCommandList)
  ├ begin/end_render_pass(RenderTargetDesc)
  ├ bind_pipeline / bind_buffers / set_push_constants
  ├ draw_indexed(indexCount, instanceCount, ...)   ← per-batch instancing もここ
  ├ dispatch(x,y,z)                                ← GPU-driven compute
  ├ write_timestamp(IRhiQueryHeap, index)          ← GpuTimerManager の下回り
  └ resource_barrier(...)

IRhiQueryHeap     — timestamp / occlusion。GpuTimerManager をこの上に載せ替える。
IRhiSwapchain     — acquire / present (VulkanContext の swapchain 部分を移管)。
IRhiBuffer/Texture — GpuAllocation を実バッキングに結ぶ。
```

`Camera` / `RenderBatch` / `ObjectDescriptor` 等の値型は API 非依存のまま
RHI 境界を渡る (内部 layout を漏らさない = Pictor CLAUDE.md の境界 OOP 方針)。

## 4. 概念マッピング (Vulkan ↔ D3D12)

| Pictor RHI | Vulkan | D3D12 |
|---|---|---|
| IRhiDevice | VkDevice + VkPhysicalDevice | ID3D12Device |
| queue | VkQueue | ID3D12CommandQueue |
| ICommandEncoder | VkCommandBuffer | ID3D12GraphicsCommandList |
| IRhiBuffer | VkBuffer + VkDeviceMemory | ID3D12Resource (buffer) |
| IRhiTexture | VkImage + View | ID3D12Resource (tex) + descriptor |
| IRhiPipeline | VkPipeline + Layout | ID3D12PipelineState + RootSignature |
| IRhiQueryHeap (ts) | VkQueryPool TIMESTAMP | ID3D12QueryHeap TIMESTAMP |
| swapchain | VkSwapchainKHR | IDXGISwapChain |
| descriptor set | VkDescriptorSet | descriptor heap + root params |
| SSBO (instance buf) | storage buffer | SRV/UAV (StructuredBuffer) |

**instancing**: 両 API とも `draw_indexed(instanceCount, firstInstance)` 同等で、
`gl_InstanceIndex` ↔ `SV_InstanceID`。本書 §6 (3D モデル instancing) と整合。

**UMA (Task 2 と合流)**: D3D12 は `D3D12_FEATURE_DATA_ARCHITECTURE` の
`UMA` / `CacheCoherentUMA` で判定でき、`analyze_device_memory()` の入力
(`DeviceMemoryDesc`) に同じ語彙でマップできる。判定ロジックは共有可能。

## 5. シェーダ戦略

現状 GLSL→SPIR-V。DX12 は DXIL が要る。選択肢:

1. **HLSL 単一ソース → DXC で SPIR-V + DXIL の両方を出す** (推奨)。DXC は両対応。
   既存 GLSL を HLSL へ移植する一括作業が要る。
2. GLSL 維持 + SPIRV-Cross で HLSL 生成 → DXC で DXIL。中間変換が増え脆い。

→ **HLSL 正本 + DXC デュアル出力**を推奨。CMake に `compile_hlsl()` を追加し
`*.hlsl` から `.spv` / `.dxil` を生成、profile/loader が backend に応じて選ぶ。

## 6. 段階的移行計画 (フェーズ)

DoD コアを壊さないため、**先に Vulkan を RHI の上へ載せ替えてから** DX12 を足す:

1. **Phase 0 — RHI 抽出**: 上記 interface を定義し、`vulkan_context` /
   `gpu_timer` / `render_pass_scheduler` の Vulkan 呼び出しを `IRhi*` の Vulkan
   実装 (`rhi/vulkan/`) へ移す。**外形の挙動は不変** (回帰のみ確認)。
2. **Phase 1 — RHI 上の Vulkan で全テスト green** を確認 (現行と等価)。
3. **Phase 2 — D3D12 実装** (`rhi/d3d12/`): IDevice/Encoder/Buffer/QueryHeap/
   Swapchain を実装。最小パス (clear + 1 instanced draw + timestamp) から。
4. **Phase 3 — HLSL 移行** + profile loader の backend 分岐。
5. **Phase 4 — UMA/ReBAR を D3D12 ARCHITECTURE で配線** (Task 2 と合流)。

各 Phase は単独で green を保ち、`PICTOR_RHI_BACKEND={vulkan,d3d12}` で選択。

## 7. リスクと評価 (decision-metrics)

- **作業コスト: 大**。RHI 抽出はレンダ経路全面に及ぶ。Phase 0/1 が最重量
  (挙動不変のリファクタ + 全回帰)。
- **AI 学習量: 高**。RHI 設計・2 API の差異・HLSL/DXC の実体験。
- **解決度**: DX12 が要る環境で根本解決。Vulkan も RHI 化で疎結合化の副益。
- **主目的一致度**: 「他アーキ対応」のうち **GPU API 軸**。CPU キャッシュ整列
  (Task 1) / UMA (Task 2) とは独立。UMA 判定語彙は §4 で合流できる。

## 8. 推奨

いきなり DX12 を書かず **Phase 0 (RHI 抽出) を独立 PR** で先行する。抽出だけで
Vulkan 経路の疎結合化という独立価値があり、DX12 はその上に安全に積める。
Phase 0 着手前にこの設計をレビューし、interface の粒度を確定する。
