#pragma once

/// GraphicsPipelineBuilder — fixed-function state を埋めた graphics pipeline
/// 生成の共通ヘルパー (`spec/rendering-extensibility-design.md` §6.3 項目6)。
///
/// 背景:
///   `ShaderRegistry::build_pipelines`（カスタムシェーダ pipeline）と
///   `PostProcessPipeline::create_pipelines_`（post-process pass pipeline）が
///   ほぼ同一の `VkGraphicsPipelineCreateInfo` 構築コードを各々ベタ書きして
///   いた。 本ヘッダはその重複を 1 つの `build_graphics_pipeline()` に統合する。
///
/// 設計:
///   - 両者の差分は小さな `GraphicsPipelineDesc` で表現する
///     (cull mode / depth test / 頂点入力レイアウト / dynamic viewport)。
///   - shader module の load / destroy は呼び出し側責務 (パスの持ち方が
///     違うため)。 本ヘルパーは module を受け取って pipeline を 1 本作る。
///   - viewport / scissor は常に dynamic state (両者とも実行時に設定する)。
///   - color attachment 1 枚 / blend 無し (両者とも同一)。 blend が要る
///     pipeline はこのヘルパー対象外。
///
/// Vulkan 依存ヘッダなので `PICTOR_HAS_VULKAN` 有効時のみ実体を持つ。

#include "pictor/core/types.h"

#ifdef PICTOR_HAS_VULKAN

#include "pictor/shader/vertex_layout.h"

#include <vulkan/vulkan.h>

namespace pictor {

/// `build_graphics_pipeline()` への入力。 ShaderRegistry / PostProcessPipeline
/// の 2 用途の差分だけを持つ。
struct GraphicsPipelineDesc {
    VkShaderModule   vert        = VK_NULL_HANDLE;   ///< 必須
    VkShaderModule   frag        = VK_NULL_HANDLE;   ///< 必須
    VkRenderPass     render_pass = VK_NULL_HANDLE;   ///< 必須
    uint32_t         subpass     = 0;
    VkPipelineLayout layout      = VK_NULL_HANDLE;   ///< 必須

    /// 頂点入力レイアウト。 空 (`vertex_layout.empty()`) なら頂点入力空 ＝
    /// fullscreen triangle / `gl_VertexIndex` 駆動 (post-process 用)。
    /// 非空なら mesh の頂点バッファ binding 0 を読む (カスタムシェーダ用)。
    VertexLayout     vertex_layout;

    /// 背面カリングを行うか。 mesh = true、 fullscreen pass = false。
    bool             cull_back   = false;
    /// 深度テスト / 書き込みを行うか。 mesh = true、 fullscreen pass = false。
    bool             depth_test  = false;
};

/// `desc` から graphics pipeline を 1 本生成する。
///
/// 成功時は `out` に pipeline を入れて true。 失敗時は false (`out` は不変)。
/// shader module / render pass / layout の破棄は呼び出し側の責務。
inline bool build_graphics_pipeline(VkDevice device,
                                    const GraphicsPipelineDesc& desc,
                                    VkPipeline& out) {
    if (desc.vert == VK_NULL_HANDLE || desc.frag == VK_NULL_HANDLE ||
        desc.render_pass == VK_NULL_HANDLE || desc.layout == VK_NULL_HANDLE)
        return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = desc.vert;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = desc.frag;
    stages[1].pName  = "main";

    // 頂点入力: 空レイアウトなら binding/attribute 共に 0。
    // vk_layout は make_create_info() が指すバッファの所有者なので、
    // pipeline 生成まで生存させる。
    const VkVertexInputLayout vk_layout = to_vk_vertex_input(desc.vertex_layout);
    const VkPipelineVertexInputStateCreateInfo vi = vk_layout.make_create_info();

    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = desc.cull_back ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dss{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (desc.depth_test) {
        dss.depthTestEnable  = VK_TRUE;
        dss.depthWriteEnable = VK_TRUE;
        dss.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;
    }

    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    ba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments    = &ba;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates    = dyn;

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount          = 2;
    gp.pStages             = stages;
    gp.pVertexInputState   = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState      = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState   = &ms;
    gp.pDepthStencilState  = &dss;
    gp.pColorBlendState    = &cb;
    gp.pDynamicState       = &dy;
    gp.layout              = desc.layout;
    gp.renderPass          = desc.render_pass;
    gp.subpass             = desc.subpass;

    return vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp,
                                     nullptr, &out) == VK_SUCCESS;
}

} // namespace pictor

#endif // PICTOR_HAS_VULKAN
