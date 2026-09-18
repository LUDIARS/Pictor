#include "vr_scene_renderer.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace vr_demo {

using pictor::float4x4;

namespace {

struct Vertex {
    float position[3];
    float normal[3];
};

constexpr uint32_t kCubeVertexCount = 36;

/// 1 辺 1 の立方体 (中心が原点)。 面ごとに法線を持つ 36 頂点。
void build_cube(Vertex out[kCubeVertexCount]) {
    // 面の法線と、 その面を張る 2 軸。
    const float faces[6][9] = {
        { 1, 0, 0,  0, 1, 0,  0, 0, 1}, {-1, 0, 0,  0, 0, 1,  0, 1, 0},
        { 0, 1, 0,  0, 0, 1,  1, 0, 0}, { 0,-1, 0,  1, 0, 0,  0, 0, 1},
        { 0, 0, 1,  1, 0, 0,  0, 1, 0}, { 0, 0,-1,  0, 1, 0,  1, 0, 0},
    };
    const float corner[6][2] = {{-1,-1}, {1,-1}, {1,1}, {-1,-1}, {1,1}, {-1,1}};
    uint32_t at = 0;
    for (const auto& f : faces) {
        for (const auto& c : corner) {
            Vertex& v = out[at++];
            for (int k = 0; k < 3; ++k) {
                v.position[k] = 0.5f * (f[k] + c[0] * f[3 + k] + c[1] * f[6 + k]);
                v.normal[k]   = f[k];
            }
        }
    }
}

float4x4 box_transform(float cx, float cy, float cz, float sx, float sy, float sz) {
    float4x4 m = float4x4::identity();
    m.m[0][0] = sx;
    m.m[1][1] = sy;
    m.m[2][2] = sz;
    m.set_translation(cx, cy, cz);
    return m;
}

} // namespace

VrSceneRenderer::~VrSceneRenderer() { shutdown(); }

bool VrSceneRenderer::initialize(VkPhysicalDevice physical, VkDevice device,
                                 VkRenderPass eye_pass, VkRenderPass multiview_pass,
                                 const std::string& shader_dir) {
    if (device_ != VK_NULL_HANDLE) return false;
    device_ = device;
    build_room_();

    Vertex cube[kCubeVertexCount];
    build_cube(cube);

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.size       = sizeof(float) * 32;  // mat4 view_proj[2]
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges    = &push;

    const bool ok =
        create_host_buffer(physical, device, sizeof(cube), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           cube_vertices_) &&
        create_host_buffer(physical, device, sizeof(Instance) * instances_.size(),
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, instance_buffer_) &&
        vkCreatePipelineLayout(device, &layout_info, nullptr, &layout_) == VK_SUCCESS &&
        create_pipeline_(eye_pass, shader_dir + "/vr_scene.vert.spv",
                         shader_dir + "/vr_scene.frag.spv", eye_pipeline_) &&
        (multiview_pass == VK_NULL_HANDLE ||
         create_pipeline_(multiview_pass, shader_dir + "/vr_scene_multiview.vert.spv",
                          shader_dir + "/vr_scene.frag.spv", multiview_pipeline_));
    if (!ok) {
        std::fprintf(stderr, "[vr_demo] failed to create the scene renderer\n");
        shutdown();
        return false;
    }
    std::memcpy(cube_vertices_.mapped, cube, sizeof(cube));
    update(0.0f);
    return true;
}

void VrSceneRenderer::shutdown() {
    if (device_ == VK_NULL_HANDLE) return;
    if (eye_pipeline_)       vkDestroyPipeline(device_, eye_pipeline_, nullptr);
    if (multiview_pipeline_) vkDestroyPipeline(device_, multiview_pipeline_, nullptr);
    if (layout_)             vkDestroyPipelineLayout(device_, layout_, nullptr);
    destroy_buffer(device_, cube_vertices_);
    destroy_buffer(device_, instance_buffer_);
    eye_pipeline_ = multiview_pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

void VrSceneRenderer::build_room_() {
    auto add = [this](const float4x4& m, float r, float g, float b) {
        instances_.push_back({m, {r, g, b, 1.0f}});
    };
    // 6m 四方・高さ 3m の部屋。 観客は中央の椅子に座り、 頭は床から約 1.2m。
    add(box_transform(0.0f, -0.05f, 0.0f, 6.0f, 0.1f, 6.0f), 0.35f, 0.24f, 0.16f);  // 床
    add(box_transform(0.0f,  3.05f, 0.0f, 6.0f, 0.1f, 6.0f), 0.85f, 0.82f, 0.76f);  // 天井
    add(box_transform(0.0f, 1.5f, -3.05f, 6.0f, 3.0f, 0.1f), 0.45f, 0.20f, 0.22f);  // 奥の壁
    add(box_transform(0.0f, 1.5f,  3.05f, 6.0f, 3.0f, 0.1f), 0.45f, 0.20f, 0.22f);  // 手前の壁
    add(box_transform(-3.05f, 1.5f, 0.0f, 0.1f, 3.0f, 6.0f), 0.40f, 0.22f, 0.24f);  // 左の壁
    add(box_transform( 3.05f, 1.5f, 0.0f, 0.1f, 3.0f, 6.0f), 0.40f, 0.22f, 0.24f);  // 右の壁
    add(box_transform(0.0f, 0.72f, -1.2f, 1.4f, 0.06f, 0.8f), 0.30f, 0.17f, 0.10f); // 机の天板
    for (float x : {-0.62f, 0.62f})
        for (float z : {-1.52f, -0.88f})
            add(box_transform(x, 0.35f, z, 0.07f, 0.7f, 0.07f), 0.24f, 0.13f, 0.08f); // 机の脚
    add(box_transform(-2.6f, 1.0f, -2.0f, 0.5f, 2.0f, 1.2f), 0.28f, 0.16f, 0.10f);  // 本棚
    add(box_transform( 2.4f, 0.45f, 1.6f, 0.9f, 0.9f, 0.9f), 0.20f, 0.30f, 0.26f);  // 箪笥
    add(box_transform( 0.0f, 1.9f, -2.95f, 1.2f, 0.9f, 0.05f), 0.75f, 0.62f, 0.30f); // 額縁
    static_count_ = static_cast<uint32_t>(instances_.size());

    // 動的層。 位置は update() が毎フレーム決める。
    add(float4x4::identity(), 0.90f, 0.75f, 0.25f);
    add(float4x4::identity(), 0.30f, 0.65f, 0.90f);
}

void VrSceneRenderer::update(float t) {
    // 机の上を回る箱と、 上下に揺れる箱。
    instances_[static_count_].model =
        box_transform(0.45f * std::cos(t), 0.85f, -1.2f + 0.25f * std::sin(t), 0.12f, 0.12f, 0.12f);
    instances_[static_count_ + 1].model =
        box_transform(1.4f, 1.2f + 0.3f * std::sin(t * 0.7f), -1.8f, 0.2f, 0.2f, 0.2f);
    // 静的層も含めて書き直す。 demo の物体数では差分更新の利得が無い。
    std::memcpy(instance_buffer_.mapped, instances_.data(), sizeof(Instance) * instances_.size());
}

bool VrSceneRenderer::create_pipeline_(VkRenderPass pass, const std::string& vert_path,
                                       const std::string& frag_path, VkPipeline& out) {
    const VkShaderModule vs = load_shader_module(device_, vert_path);
    const VkShaderModule fs = load_shader_module(device_, frag_path);
    auto destroy_modules = [&]() {
        if (vs) vkDestroyShaderModule(device_, vs, nullptr);
        if (fs) vkDestroyShaderModule(device_, fs, nullptr);
    };
    if (!vs || !fs) {
        destroy_modules();
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName  = "main";

    const VkVertexInputBindingDescription bindings[2] = {
        {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE},
    };
    VkVertexInputAttributeDescription attributes[7] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
    };
    for (uint32_t column = 0; column < 4; ++column) {
        attributes[2 + column] = {2 + column, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                  static_cast<uint32_t>(sizeof(float) * 4 * column)};
    }
    attributes[6] = {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                     static_cast<uint32_t>(offsetof(Instance, color))};

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount   = 2;
    vertex_input.pVertexBindingDescriptions      = bindings;
    vertex_input.vertexAttributeDescriptionCount = 7;
    vertex_input.pVertexAttributeDescriptions    = attributes;

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = VK_CULL_MODE_NONE;  // 部屋は内側から見るので面の向きで捨てない
    raster.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable  = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    // LESS が差分描画の要。 再投影で埋まった画素は深度で弾かれ、 穴だけが描かれる。
    depth.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments    = &blend_attachment;

    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates    = dynamic_states;

    VkGraphicsPipelineCreateInfo info{};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount          = 2;
    info.pStages             = stages;
    info.pVertexInputState   = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState      = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState   = &multisample;
    info.pDepthStencilState  = &depth;
    info.pColorBlendState    = &blend;
    info.pDynamicState       = &dynamic;
    info.layout              = layout_;
    info.renderPass          = pass;

    const VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &out);
    destroy_modules();
    if (result != VK_SUCCESS) out = VK_NULL_HANDLE;
    return result == VK_SUCCESS;
}

void VrSceneRenderer::record_(VkCommandBuffer cmd, VkPipeline pipeline, Layer layer,
                              const float4x4* view_proj, uint32_t matrix_count,
                              VkExtent2D extent) const {
    if (pipeline == VK_NULL_HANDLE) return;
    const uint32_t first = layer == Layer::Static ? 0 : static_count_;
    const uint32_t count = layer == Layer::Static
        ? static_count_
        : static_cast<uint32_t>(instances_.size()) - static_count_;
    if (count == 0) return;

    VkViewport viewport{};
    viewport.width    = static_cast<float>(extent.width);
    viewport.height   = static_cast<float>(extent.height);
    viewport.maxDepth = 1.0f;
    const VkRect2D scissor{{0, 0}, extent};

    const VkBuffer     buffers[2] = {cube_vertices_.buffer, instance_buffer_.buffer};
    const VkDeviceSize offsets[2] = {0, 0};

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(float) * 16 * matrix_count, view_proj);
    vkCmdDraw(cmd, kCubeVertexCount, count, 0, first);
}

void VrSceneRenderer::draw(VkCommandBuffer cmd, Layer layer, const float4x4& view_proj,
                           VkExtent2D extent) const {
    record_(cmd, eye_pipeline_, layer, &view_proj, 1, extent);
}

void VrSceneRenderer::draw_multiview(VkCommandBuffer cmd, Layer layer,
                                     const float4x4 view_proj[2], VkExtent2D extent) const {
    record_(cmd, multiview_pipeline_, layer, view_proj, 2, extent);
}

} // namespace vr_demo
