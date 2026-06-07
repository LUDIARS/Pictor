# Shader — カスタムシェーダ登録 + pipeline 生成

> 実装: `include/pictor/shader/` (3), `src/shader/`

Visus CUSTOM kind のカスタムシェーダを登録し、Vulkan graphics pipeline を生成する最小レジストリ。
SPIR-V は **事前コンパイル済**を前提 (Pictor は runtime コンパイルしない)。

## 構成

| クラス / struct | 役割 |
|---|---|
| `ShaderRegistry` | カスタム vert+frag SPIR-V を受け取り on-demand で pipeline 生成・handle 保持。Phase 1 は vert+frag 固定 (compute/permutation は Phase 2)。`PICTOR_HAS_VULKAN` 条件 |
| `CustomShaderDef` | 1 ペアの定義: name / vert_spv / frag_spv / vertex_layout (空=Phase1 gl_VertexIndex 駆動) |
| `GraphicsPipelineDesc` + `build_graphics_pipeline()` | pipeline 生成の共通記述子 (ShaderRegistry と PostProcessPipeline で共用) |
| vertex_layout 変換 | `to_vk_format()` / `to_vk_vertex_input()` — agnostic な `VertexLayout` を Vulkan の binding/attribute へ |

## 主要 API

```cpp
ShaderHandle register_shader(CustomShaderDef def);
const CustomShaderDef* get(ShaderHandle) const;
// Vulkan のみ
bool build_pipelines(VulkanContext&, VkRenderPass, uint32_t subpass, VkPipelineLayout);
VkPipeline pipeline(ShaderHandle) const;     // O(1)
```

## SPIR-V / pipeline

- **事前コンパイル**: `.spv` を `vkCreateShaderModule` へ。runtime の shaderc/glslang 依存なし (コンパイルは Ergo tools 等の asset pipeline 責務)
- `build_pipelines()` を 1 回呼んで全 handle 分の pipeline を同期生成。`pipeline(handle)` は配列引き
- stage: vertex (binding0 mesh VB) → fragment (color attachment0、blend なし)、backface cull + depth test/write on、viewport/scissor は dynamic
- hot-reload は Phase 1 非対応 (再 register + rebuild)

## 設計シーム

```
Visus CUSTOM → VisusDesc.shader_stages[] (vert/frag spv path)
  → ShaderRegistry::register_shader() → ShaderHandle
  → ObjectDescriptor.customShader → ShaderKey (bit63=custom, 32-62=handle)
  → DrawCommand.shader_key → render loop が pipeline(handle) を lookup + vkCmdBindPipeline
```

## 依存 / 位置

`vulkan/vulkan.h` (`PICTOR_HAS_VULKAN` 条件、headless test は省略)、`VulkanContext`。フレームでは **render setup で 1 回 init**、record 時に host が `ShaderKey::is_custom()` で判定して既定 PBR の代わりに bind。関連: [surface.md](surface.md) / Visus (`spec/subsystem/` 外、`include/pictor/visus/`)。
