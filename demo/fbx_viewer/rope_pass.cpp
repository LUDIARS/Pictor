#include "rope_pass.h"

#include "vk_buffer_util.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace pictor_fbx_viewer {

using pictor::float3;

namespace {

constexpr uint32_t kRopeSides = 10;

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

/// Sweep a ring of kRopeSides vertices along `poly`, using parallel
/// transport so the tube does not twist at sharp bends.
void sweep_tube(const std::vector<float3>& poly, size_t point_limit, float radius,
                std::vector<RopeVertex>& verts, std::vector<uint32_t>& idx) {
    const size_t n = std::min(poly.size(), point_limit);
    if (n < 2) return;

    float3 tangent = normalize(sub(poly[1], poly[0]));
    float3 ref = std::fabs(tangent.y) < 0.9f ? float3{0, 1, 0} : float3{1, 0, 0};
    float3 side = normalize(cross(tangent, ref));
    float3 up   = cross(side, tangent);
    float  arc  = 0.0f;

    const uint32_t base = static_cast<uint32_t>(verts.size());
    for (size_t i = 0; i < n; ++i) {
        if (i > 0) {
            float3 next_t = normalize(sub(poly[i], poly[i - 1]));
            if (i + 1 < n) next_t = normalize(add(next_t, normalize(sub(poly[i + 1], poly[i]))));
            // Parallel transport: re-project the side vector onto the new normal plane.
            side = normalize(sub(side, mul(next_t, dot(side, next_t))));
            up   = cross(side, next_t);
            tangent = next_t;
            arc += std::sqrt(dot(sub(poly[i], poly[i - 1]), sub(poly[i], poly[i - 1])));
        }
        for (uint32_t k = 0; k < kRopeSides; ++k) {
            float a = 2.0f * 3.14159265f * static_cast<float>(k) / kRopeSides;
            float3 nrm = add(mul(side, std::cos(a)), mul(up, std::sin(a)));
            float3 p = add(poly[i], mul(nrm, radius));
            RopeVertex v{};
            v.position[0] = p.x; v.position[1] = p.y; v.position[2] = p.z;
            v.normal[0] = nrm.x; v.normal[1] = nrm.y; v.normal[2] = nrm.z;
            v.uv[0] = arc / std::max(radius, 1e-4f);   // stripes scale with thickness
            v.uv[1] = static_cast<float>(k) / kRopeSides;
            verts.push_back(v);
        }
    }
    for (size_t i = 0; i + 1 < n; ++i) {
        for (uint32_t k = 0; k < kRopeSides; ++k) {
            uint32_t k2 = (k + 1) % kRopeSides;
            uint32_t a = base + static_cast<uint32_t>(i) * kRopeSides + k;
            uint32_t b = base + static_cast<uint32_t>(i) * kRopeSides + k2;
            uint32_t c = a + kRopeSides;
            uint32_t d = b + kRopeSides;
            idx.insert(idx.end(), {a, c, b, b, c, d});
        }
    }
}

} // namespace

bool RopePass::create(const CreateInfo& ci) {
    device_ = ci.device;
    physical_device_ = ci.physical_device;
    VkDevice d = device_;

    VkShaderModule vs = load_shader_spv(d, ci.shader_dir + "/rope.vert.spv");
    VkShaderModule fs = load_shader_spv(d, ci.shader_dir + "/rope.frag.spv");
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
    binding.binding = 0; binding.stride = sizeof(RopeVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = 0;
    attrs[1].location = 1; attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[1].offset = 12;
    attrs[2].location = 2; attrs[2].format = VK_FORMAT_R32G32_SFLOAT;    attrs[2].offset = 24;
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
    rs.cullMode    = VK_CULL_MODE_BACK_BIT;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
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
        std::fprintf(stderr, "[rope] pipeline creation failed (%d)\n", static_cast<int>(r));
        vkDestroyPipelineLayout(d, pipeline_layout_, nullptr);
        pipeline_layout_ = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool RopePass::build_geometry(const NawaCapsuleChain& chain, float rope_radius,
                              uint32_t visible_segments) {
    std::vector<RopeVertex> verts;
    std::vector<uint32_t>   idx;
    // Segments are interleaved by strand in NawaCapsuleChain. Give each
    // strand its exact share of that prefix so geometry and deformation
    // reveal the same segments during wrap animation.
    const size_t strand_count = std::max<size_t>(chain.strands.size(), 1);
    for (size_t strand = 0; strand < chain.strands.size(); ++strand) {
        const size_t shown_segments = visible_segments / strand_count +
                                      (strand < visible_segments % strand_count ? 1 : 0);
        sweep_tube(chain.strands[strand], shown_segments + 1, rope_radius, verts, idx);
    }

    const uint32_t replacement_index_count = static_cast<uint32_t>(idx.size());
    if (replacement_index_count == 0) {
        vkDeviceWaitIdle(device_);
        if (vb_) vkDestroyBuffer(device_, vb_, nullptr);
        if (vb_mem_) vkFreeMemory(device_, vb_mem_, nullptr);
        if (ib_) vkDestroyBuffer(device_, ib_, nullptr);
        if (ib_mem_) vkFreeMemory(device_, ib_mem_, nullptr);
        vb_ = VK_NULL_HANDLE; vb_mem_ = VK_NULL_HANDLE;
        ib_ = VK_NULL_HANDLE; ib_mem_ = VK_NULL_HANDLE;
        index_count_ = 0;
        return true;
    }

    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkDeviceSize vb_size = verts.size() * sizeof(RopeVertex);
    VkDeviceSize ib_size = idx.size() * sizeof(uint32_t);
    VkBuffer replacement_vb = VK_NULL_HANDLE, replacement_ib = VK_NULL_HANDLE;
    VkDeviceMemory replacement_vb_mem = VK_NULL_HANDLE, replacement_ib_mem = VK_NULL_HANDLE;
    if (!create_buffer(device_, physical_device_, vb_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       hv, replacement_vb, replacement_vb_mem)) return false;
    if (!create_buffer(device_, physical_device_, ib_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                       hv, replacement_ib, replacement_ib_mem)) {
        vkDestroyBuffer(device_, replacement_vb, nullptr);
        vkFreeMemory(device_, replacement_vb_mem, nullptr);
        return false;
    }
    auto discard_replacement = [&]() {
        if (replacement_vb) vkDestroyBuffer(device_, replacement_vb, nullptr);
        if (replacement_vb_mem) vkFreeMemory(device_, replacement_vb_mem, nullptr);
        if (replacement_ib) vkDestroyBuffer(device_, replacement_ib, nullptr);
        if (replacement_ib_mem) vkFreeMemory(device_, replacement_ib_mem, nullptr);
    };
    void* p = nullptr;
    if (vkMapMemory(device_, replacement_vb_mem, 0, vb_size, 0, &p) != VK_SUCCESS) {
        discard_replacement();
        return false;
    }
    std::memcpy(p, verts.data(), vb_size);
    vkUnmapMemory(device_, replacement_vb_mem);
    if (vkMapMemory(device_, replacement_ib_mem, 0, ib_size, 0, &p) != VK_SUCCESS) {
        discard_replacement();
        return false;
    }
    std::memcpy(p, idx.data(), ib_size);
    vkUnmapMemory(device_, replacement_ib_mem);

    vkDeviceWaitIdle(device_);
    if (vb_) vkDestroyBuffer(device_, vb_, nullptr);
    if (vb_mem_) vkFreeMemory(device_, vb_mem_, nullptr);
    if (ib_) vkDestroyBuffer(device_, ib_, nullptr);
    if (ib_mem_) vkFreeMemory(device_, ib_mem_, nullptr);
    vb_ = replacement_vb; vb_mem_ = replacement_vb_mem;
    ib_ = replacement_ib; ib_mem_ = replacement_ib_mem;
    index_count_ = replacement_index_count;
    return true;
}

bool RopePass::upload(DynMesh& mesh, const std::vector<RopeVertex>& verts,
                      const std::vector<uint32_t>& idx, bool /*host_visible_resize*/) {
    const uint32_t vc = static_cast<uint32_t>(verts.size());
    const uint32_t ic = static_cast<uint32_t>(idx.size());
    if (vc == 0 || ic == 0) return true;
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (vc > mesh.vb_capacity || ic > mesh.ib_capacity) {
        // Grow with headroom and fully upload the replacement before retiring
        // geometry that may still be displayed.
        DynMesh replacement;
        replacement.vb_capacity = vc * 2;
        replacement.ib_capacity = ic * 2;
        if (!create_buffer(device_, physical_device_,
                           static_cast<VkDeviceSize>(replacement.vb_capacity) * sizeof(RopeVertex),
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, hv,
                           replacement.vb, replacement.vb_mem)) return false;
        if (!create_buffer(device_, physical_device_,
                           static_cast<VkDeviceSize>(replacement.ib_capacity) * sizeof(uint32_t),
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT, hv,
                           replacement.ib, replacement.ib_mem)) {
            destroy_mesh(replacement);
            return false;
        }
        void* replacement_map = nullptr;
        if (vkMapMemory(device_, replacement.vb_mem, 0, vc * sizeof(RopeVertex), 0,
                        &replacement_map) != VK_SUCCESS) {
            destroy_mesh(replacement);
            return false;
        }
        std::memcpy(replacement_map, verts.data(), vc * sizeof(RopeVertex));
        vkUnmapMemory(device_, replacement.vb_mem);
        if (vkMapMemory(device_, replacement.ib_mem, 0, ic * sizeof(uint32_t), 0,
                        &replacement_map) != VK_SUCCESS) {
            destroy_mesh(replacement);
            return false;
        }
        std::memcpy(replacement_map, idx.data(), ic * sizeof(uint32_t));
        vkUnmapMemory(device_, replacement.ib_mem);

        vkDeviceWaitIdle(device_);
        destroy_mesh(mesh);
        mesh = replacement;
        return true;
    }
    void* p = nullptr;
    if (vkMapMemory(device_, mesh.vb_mem, 0, vc * sizeof(RopeVertex), 0, &p) != VK_SUCCESS) return false;
    std::memcpy(p, verts.data(), vc * sizeof(RopeVertex));
    vkUnmapMemory(device_, mesh.vb_mem);
    if (vkMapMemory(device_, mesh.ib_mem, 0, ic * sizeof(uint32_t), 0, &p) != VK_SUCCESS) return false;
    std::memcpy(p, idx.data(), ic * sizeof(uint32_t));
    vkUnmapMemory(device_, mesh.ib_mem);
    return true;
}

bool RopePass::update_tail(const std::vector<float3>& polyline, float rope_radius) {
    std::vector<RopeVertex> verts;
    std::vector<uint32_t>   idx;
    sweep_tube(polyline, polyline.size(), rope_radius, verts, idx);
    const uint32_t replacement_index_count = static_cast<uint32_t>(idx.size());
    if (replacement_index_count == 0) {
        tail_index_count_ = 0;
        return true;
    }
    if (!upload(tail_, verts, idx, true)) return false;
    tail_index_count_ = replacement_index_count;
    return true;
}

void RopePass::record(VkCommandBuffer cmd, VkDescriptorSet scene_set) const {
    if (!valid()) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                            0, 1, &scene_set, 0, nullptr);
    VkDeviceSize off = 0;
    if (index_count_ > 0) {
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb_, &off);
        vkCmdBindIndexBuffer(cmd, ib_, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, index_count_, 1, 0, 0, 0);
    }
    if (tail_index_count_ > 0 && tail_.vb) {
        vkCmdBindVertexBuffers(cmd, 0, 1, &tail_.vb, &off);
        vkCmdBindIndexBuffer(cmd, tail_.ib, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, tail_index_count_, 1, 0, 0, 0);
    }
}

void RopePass::destroy_mesh(DynMesh& m) {
    if (m.vb)     { vkDestroyBuffer(device_, m.vb, nullptr);  m.vb = VK_NULL_HANDLE; }
    if (m.vb_mem) { vkFreeMemory(device_, m.vb_mem, nullptr); m.vb_mem = VK_NULL_HANDLE; }
    if (m.ib)     { vkDestroyBuffer(device_, m.ib, nullptr);  m.ib = VK_NULL_HANDLE; }
    if (m.ib_mem) { vkFreeMemory(device_, m.ib_mem, nullptr); m.ib_mem = VK_NULL_HANDLE; }
    m.vb_capacity = m.ib_capacity = 0;
}

void RopePass::destroy_buffers() {
    destroy_mesh(tail_);
    tail_index_count_ = 0;
    if (vb_)     { vkDestroyBuffer(device_, vb_, nullptr);  vb_ = VK_NULL_HANDLE; }
    if (vb_mem_) { vkFreeMemory(device_, vb_mem_, nullptr); vb_mem_ = VK_NULL_HANDLE; }
    if (ib_)     { vkDestroyBuffer(device_, ib_, nullptr);  ib_ = VK_NULL_HANDLE; }
    if (ib_mem_) { vkFreeMemory(device_, ib_mem_, nullptr); ib_mem_ = VK_NULL_HANDLE; }
    index_count_ = 0;
}

void RopePass::destroy() {
    if (!device_) return;
    destroy_buffers();
    if (pipeline_)        { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (pipeline_layout_) { vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr); pipeline_layout_ = VK_NULL_HANDLE; }
    device_ = VK_NULL_HANDLE;
}

} // namespace pictor_fbx_viewer
