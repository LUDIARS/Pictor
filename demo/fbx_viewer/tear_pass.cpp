#include "tear_pass.h"

#include "vk_buffer_util.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace pictor_fbx_viewer {

using pictor::float3;

namespace {

float3 sub(float3 a, float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float3 add(float3 a, float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
float3 mul(float3 a, float s)  { return {a.x * s, a.y * s, a.z * s}; }
float  dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float3 cross(float3 a, float3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float3 normalize(float3 a) {
    float l = std::sqrt(dot(a, a));
    return l > 1e-8f ? mul(a, 1.0f / l) : float3{0, 1, 0};
}

} // namespace

bool TearPass::create(const CreateInfo& ci) {
    device_ = ci.device;
    physical_device_ = ci.physical_device;
    VkDevice d = device_;

    VkShaderModule vs = load_shader_spv(d, ci.shader_dir + "/tear.vert.spv");
    VkShaderModule fs = load_shader_spv(d, ci.shader_dir + "/tear.frag.spv");
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(d, vs, nullptr);
        if (fs) vkDestroyShaderModule(d, fs, nullptr);
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0; binding.stride = sizeof(TearVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = 0;
    attrs[1].location = 1; attrs[1].format = VK_FORMAT_R32G32_SFLOAT;    attrs[1].offset = 12;
    attrs[2].location = 2; attrs[2].format = VK_FORMAT_R32_SFLOAT;       attrs[2].offset = 20;
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 3; vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // Tears sit on the surface: depth-tested so the far side hides them,
    // no depth write so overlapping drops blend.
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dyn_states;

    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1; pl.pSetLayouts = &ci.scene_layout;
    if (vkCreatePipelineLayout(d, &pl, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(d, vs, nullptr);
        vkDestroyShaderModule(d, fs, nullptr);
        return false;
    }
    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2; info.pStages = stages;
    info.pVertexInputState = &vi; info.pInputAssemblyState = &ia;
    info.pViewportState = &vp; info.pRasterizationState = &rs;
    info.pMultisampleState = &ms; info.pDepthStencilState = &ds;
    info.pColorBlendState = &cb; info.pDynamicState = &dyn;
    info.layout = pipeline_layout_; info.renderPass = ci.render_pass;
    VkResult r = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
    vkDestroyShaderModule(d, vs, nullptr);
    vkDestroyShaderModule(d, fs, nullptr);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[tears] pipeline creation failed (%d)\n", static_cast<int>(r));
        vkDestroyPipelineLayout(d, pipeline_layout_, nullptr);
        pipeline_layout_ = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool TearPass::ensure_capacity(uint32_t vertex_count) {
    if (vertex_count <= capacity_) return true;
    const uint32_t new_capacity = vertex_count * 2;
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkBuffer replacement = VK_NULL_HANDLE;
    VkDeviceMemory replacement_mem = VK_NULL_HANDLE;
    if (!create_buffer(device_, physical_device_,
                       static_cast<VkDeviceSize>(new_capacity) * sizeof(TearVertex),
                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, hv,
                       replacement, replacement_mem)) return false;

    // Validate host mapping before retiring geometry that is still usable.
    void* mapped = nullptr;
    if (vkMapMemory(device_, replacement_mem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        vkDestroyBuffer(device_, replacement, nullptr);
        vkFreeMemory(device_, replacement_mem, nullptr);
        return false;
    }
    vkUnmapMemory(device_, replacement_mem);

    vkDeviceWaitIdle(device_);
    if (vb_) vkDestroyBuffer(device_, vb_, nullptr);
    if (vb_mem_) vkFreeMemory(device_, vb_mem_, nullptr);
    vb_ = replacement;
    vb_mem_ = replacement_mem;
    capacity_ = new_capacity;
    return true;
}

void TearPass::update(float time_s,
                      const std::vector<TearEmitter>& emitters,
                      const float3& camera_pos,
                      const SurfaceProjectFn& project,
                      const Params& p) {
    std::vector<TearVertex> verts;
    verts.reserve(emitters.size() * p.drops_per_eye * 6);
    const float deg = 3.14159265f / 180.0f;

    for (size_t e = 0; e < emitters.size(); ++e) {
        const TearEmitter& em = emitters[e];
        const float3 n = normalize(em.normal);
        // Outward axis in the tangent plane; world up projected gives the fan's "up".
        float3 out_axis = sub(em.outward, mul(n, dot(em.outward, n)));
        if (dot(out_axis, out_axis) < 1e-8f) out_axis = cross(n, float3{0, 1, 0});
        out_axis = normalize(out_axis);
        float3 up_axis = normalize(sub(float3{0, 1, 0}, mul(n, n.y)));

        const float angle = p.line_tilt_deg * deg;
        const float3 dir = add(mul(out_axis, std::cos(angle)), mul(up_axis, std::sin(angle)));

        for (uint32_t k = 0; k < p.drops_per_eye; ++k) {
            // Slot k runs the line with phase offset k/N so the drops stay
            // evenly spaced while streaming away from the eye.
            const float slot = static_cast<float>(k) / static_cast<float>(std::max<uint32_t>(p.drops_per_eye, 1));
            float run = time_s * p.flow_hz + slot + static_cast<float>(e) * 0.5f;
            run -= std::floor(run);   // 0..1 along the line
            // Tremble: small in-place jitter so the line does not look mechanical.
            const float phase = static_cast<float>(e) * 1.7f + static_cast<float>(k) * 2.1f;
            const float wob = std::sin(time_s * p.tremble_hz * 6.2831853f + phase);
            const float dist = p.line_start + p.line_length * run;
            float3 pos = add(em.position, mul(dir, dist));
            pos = add(pos, mul(up_axis, p.tremble_amp * p.drop_size * wob));
            float3 nrm = n;
            if (project) {
                float3 on_surface, n2;
                if (project(pos, on_surface, n2)) { pos = on_surface; nrm = n2; }
            }
            pos = add(pos, mul(nrm, p.surface_lift));

            // Billboard whose +Y (the teardrop tip) points back toward the eye.
            const float3 view = normalize(sub(camera_pos, pos));
            float3 to_eye = sub(em.position, pos);
            to_eye = sub(to_eye, mul(view, dot(to_eye, view)));   // flatten into the view plane
            float3 up = dot(to_eye, to_eye) > 1e-8f ? normalize(to_eye) : float3{0, 1, 0};
            float3 right = normalize(cross(up, view));
            // Drops shrink slightly as they get farther from the eye.
            const float half_w = p.drop_size * (1.05f - 0.25f * run);
            const float half_h = half_w * 1.25f;

            const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            const int order[6] = {0, 1, 2, 0, 2, 3};
            TearVertex quad[4];
            for (int c = 0; c < 4; ++c) {
                float3 wp = add(pos, add(mul(right, corners[c][0] * half_w), mul(up, corners[c][1] * half_h)));
                quad[c].position[0] = wp.x; quad[c].position[1] = wp.y; quad[c].position[2] = wp.z;
                quad[c].corner[0] = corners[c][0]; quad[c].corner[1] = corners[c][1];
                quad[c].age = run; quad[c].pad = 0.0f;
            }
            for (int i : order) verts.push_back(quad[i]);
        }
    }

    vertex_count_ = static_cast<uint32_t>(verts.size());
    if (vertex_count_ == 0) return;
    if (!ensure_capacity(vertex_count_)) { vertex_count_ = 0; return; }
    void* mapped = nullptr;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(vertex_count_) * sizeof(TearVertex);
    if (vkMapMemory(device_, vb_mem_, 0, bytes, 0, &mapped) != VK_SUCCESS) {
        vertex_count_ = 0;
        return;
    }
    std::memcpy(mapped, verts.data(), bytes);
    vkUnmapMemory(device_, vb_mem_);
}

void TearPass::record(VkCommandBuffer cmd, VkDescriptorSet scene_set) const {
    if (!valid() || vertex_count_ == 0 || vb_ == VK_NULL_HANDLE) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                            0, 1, &scene_set, 0, nullptr);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb_, &off);
    vkCmdDraw(cmd, vertex_count_, 1, 0, 0);
}

void TearPass::destroy() {
    if (!device_) return;
    if (vb_)     { vkDestroyBuffer(device_, vb_, nullptr);  vb_ = VK_NULL_HANDLE; }
    if (vb_mem_) { vkFreeMemory(device_, vb_mem_, nullptr); vb_mem_ = VK_NULL_HANDLE; }
    if (pipeline_)        { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (pipeline_layout_) { vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr); pipeline_layout_ = VK_NULL_HANDLE; }
    device_ = VK_NULL_HANDLE;
    capacity_ = 0; vertex_count_ = 0;
}

} // namespace pictor_fbx_viewer
