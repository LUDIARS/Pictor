#include "pictor/surface/simple_renderer.h"

#ifdef PICTOR_HAS_VULKAN

#include "pictor/surface/vulkan_context.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <vector>
#include <array>
#include <unordered_map>
#include <string>

namespace pictor {

// UBO layout matching the shader
struct SceneUBO {
    float view[16];
    float proj[16];
    float lightDir[4];
};

SimpleRenderer::~SimpleRenderer() {
    if (initialized_) shutdown();
}

bool SimpleRenderer::initialize(VulkanContext& vk_ctx, const char* shader_dir) {
    vk_ctx_ = &vk_ctx;
    device_ = vk_ctx.device();

    if (!create_mesh_buffers())       return false;
    if (!create_instance_buffer())    return false;
    if (!create_descriptor_sets())    return false;
    if (!create_pipeline(shader_dir)) return false;

    initialized_ = true;
    printf("[SimpleRenderer] Initialized (max %u instances, %u tris/sphere)\n",
           MAX_INSTANCES, index_count_ / 3);
    return true;
}

void SimpleRenderer::shutdown() {
    if (!initialized_) return;
    vkDeviceWaitIdle(device_);

    if (pipeline_)        vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (desc_pool_)       vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
    if (desc_set_layout_) vkDestroyDescriptorSetLayout(device_, desc_set_layout_, nullptr);

    auto destroy_buf = [&](VkBuffer& b, VkDeviceMemory& m) {
        if (b) vkDestroyBuffer(device_, b, nullptr);
        if (m) vkFreeMemory(device_, m, nullptr);
        b = VK_NULL_HANDLE; m = VK_NULL_HANDLE;
    };
    destroy_buf(vertex_buffer_, vertex_memory_);
    destroy_buf(index_buffer_, index_memory_);
    destroy_buf(ubo_buffer_, ubo_memory_);
    destroy_buf(instance_buffer_, instance_memory_);

    initialized_ = false;
}

void SimpleRenderer::update_instances(const float* data, uint32_t count) {
    if (count > MAX_INSTANCES) count = MAX_INSTANCES;
    instance_count_ = count;

    VkDeviceSize size = count * 4 * sizeof(float);
    if (size == 0) return;

    void* mapped = nullptr;
    vkMapMemory(device_, instance_memory_, 0, size, 0, &mapped);
    memcpy(mapped, data, size);
    vkUnmapMemory(device_, instance_memory_);
}

void SimpleRenderer::render(VkCommandBuffer cmd, VkRenderPass render_pass,
                            VkFramebuffer framebuffer, VkExtent2D extent,
                            const float* view, const float* proj) {
    if (!initialized_ || instance_count_ == 0) return;

    // Update UBO
    SceneUBO ubo{};
    memcpy(ubo.view, view, 16 * sizeof(float));
    memcpy(ubo.proj, proj, 16 * sizeof(float));
    ubo.lightDir[0] = 0.4f;
    ubo.lightDir[1] = 0.8f;
    ubo.lightDir[2] = 0.3f;
    ubo.lightDir[3] = 0.0f;

    void* mapped = nullptr;
    vkMapMemory(device_, ubo_memory_, 0, sizeof(ubo), 0, &mapped);
    memcpy(mapped, &ubo, sizeof(ubo));
    vkUnmapMemory(device_, ubo_memory_);

    // Begin render pass
    VkClearValue clear_values[2];
    clear_values[0].color = {{0.02f, 0.02f, 0.05f, 1.0f}};
    clear_values[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rp_info{};
    rp_info.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_info.renderPass  = render_pass;
    rp_info.framebuffer = framebuffer;
    rp_info.renderArea  = {{0, 0}, extent};
    rp_info.clearValueCount = 1;
    rp_info.pClearValues    = clear_values;

    vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_layout_, 0, 1, &desc_set_, 0, nullptr);

    VkViewport viewport{};
    viewport.x = 0; viewport.y = 0;
    viewport.width  = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_, &offset);
    vkCmdBindIndexBuffer(cmd, index_buffer_, 0, VK_INDEX_TYPE_UINT32);

    vkCmdDrawIndexed(cmd, index_count_, instance_count_, 0, 0, 0);

    vkCmdEndRenderPass(cmd);
}

// ---- Pipeline creation ----

VkShaderModule SimpleRenderer::load_shader(const char* path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        fprintf(stderr, "[SimpleRenderer] Cannot open shader: %s\n", path);
        return VK_NULL_HANDLE;
    }

    size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> code(size);
    file.seekg(0);
    file.read(code.data(), static_cast<std::streamsize>(size));

    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = size;
    info.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &info, nullptr, &mod) != VK_SUCCESS) {
        fprintf(stderr, "[SimpleRenderer] Failed to create shader module: %s\n", path);
    }
    return mod;
}

bool SimpleRenderer::create_pipeline(const char* shader_dir) {
    std::string vert_path = std::string(shader_dir) + "/simple_inst.vert.spv";
    std::string frag_path = std::string(shader_dir) + "/simple_inst.frag.spv";

    VkShaderModule vert = load_shader(vert_path.c_str());
    VkShaderModule frag = load_shader(frag_path.c_str());
    if (!vert || !frag) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    // Vertex input: pos(vec3) + normal(vec3)
    VkVertexInputBindingDescription bind{};
    bind.binding   = 0;
    bind.stride    = sizeof(Vertex);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0;
    attrs[0].binding  = 0;
    attrs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset   = offsetof(Vertex, pos);
    attrs[1].location = 1;
    attrs[1].binding  = 0;
    attrs[1].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset   = offsetof(Vertex, normal);

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount   = 1;
    vertex_input.pVertexBindingDescriptions      = &bind;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = VK_CULL_MODE_BACK_BIT;
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments    = &blend_att;

    VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dyn_states;

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts    = &desc_set_layout_;

    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        fprintf(stderr, "[SimpleRenderer] Failed to create pipeline layout\n");
        vkDestroyShaderModule(device_, vert, nullptr);
        vkDestroyShaderModule(device_, frag, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount          = 2;
    pi.pStages             = stages;
    pi.pVertexInputState   = &vertex_input;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState      = &vp;
    pi.pRasterizationState = &raster;
    pi.pMultisampleState   = &ms;
    pi.pDepthStencilState  = &ds;
    pi.pColorBlendState    = &blend;
    pi.pDynamicState       = &dyn;
    pi.layout              = pipeline_layout_;
    pi.renderPass          = vk_ctx_->default_render_pass();
    pi.subpass             = 0;

    VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline_);

    vkDestroyShaderModule(device_, vert, nullptr);
    vkDestroyShaderModule(device_, frag, nullptr);

    if (result != VK_SUCCESS) {
        fprintf(stderr, "[SimpleRenderer] Failed to create graphics pipeline\n");
        return false;
    }

    return true;
}

// ---- Mesh generation: icosphere ----

void SimpleRenderer::generate_icosphere(std::vector<Vertex>& vertices,
                                         std::vector<uint32_t>& indices) {
    // Base icosahedron
    const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;

    std::vector<std::array<float, 3>> verts = {
        {-1, t, 0}, { 1, t, 0}, {-1,-t, 0}, { 1,-t, 0},
        { 0,-1, t}, { 0, 1, t}, { 0,-1,-t}, { 0, 1,-t},
        { t, 0,-1}, { t, 0, 1}, {-t, 0,-1}, {-t, 0, 1}
    };
    // Normalize to unit sphere
    for (auto& v : verts) {
        float len = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        v[0] /= len; v[1] /= len; v[2] /= len;
    }

    std::vector<std::array<uint32_t, 3>> tris = {
        {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
        {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
        {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
        {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}
    };

    // Subdivide once for level-1 icosphere (42 verts, 80 tris)
    auto midpoint = [&](uint32_t a, uint32_t b) -> uint32_t {
        float mx = (verts[a][0] + verts[b][0]) * 0.5f;
        float my = (verts[a][1] + verts[b][1]) * 0.5f;
        float mz = (verts[a][2] + verts[b][2]) * 0.5f;
        float len = std::sqrt(mx*mx + my*my + mz*mz);
        mx /= len; my /= len; mz /= len;
        uint32_t idx = static_cast<uint32_t>(verts.size());
        verts.push_back({mx, my, mz});
        return idx;
    };

    std::vector<std::array<uint32_t, 3>> new_tris;
    // Use a cache to avoid duplicate midpoints
    std::unordered_map<uint64_t, uint32_t> cache;
    auto get_mid = [&](uint32_t a, uint32_t b) -> uint32_t {
        uint64_t key = (a < b) ? ((uint64_t)a << 32 | b) : ((uint64_t)b << 32 | a);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        uint32_t m = midpoint(a, b);
        cache[key] = m;
        return m;
    };

    for (auto& tri : tris) {
        uint32_t a = tri[0], b = tri[1], c = tri[2];
        uint32_t ab = get_mid(a, b);
        uint32_t bc = get_mid(b, c);
        uint32_t ca = get_mid(c, a);
        new_tris.push_back({a, ab, ca});
        new_tris.push_back({b, bc, ab});
        new_tris.push_back({c, ca, bc});
        new_tris.push_back({ab, bc, ca});
    }
    tris = new_tris;

    // Build output
    vertices.resize(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) {
        vertices[i].pos[0] = verts[i][0] * 0.5f;  // radius 0.5
        vertices[i].pos[1] = verts[i][1] * 0.5f;
        vertices[i].pos[2] = verts[i][2] * 0.5f;
        vertices[i].normal[0] = verts[i][0]; // unit normal
        vertices[i].normal[1] = verts[i][1];
        vertices[i].normal[2] = verts[i][2];
    }

    indices.clear();
    for (auto& tri : tris) {
        indices.push_back(tri[0]);
        indices.push_back(tri[1]);
        indices.push_back(tri[2]);
    }
}

// ---- Buffer helpers ----

uint32_t SimpleRenderer::find_memory_type(uint32_t type_filter,
                                           VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(vk_ctx_->physical_device(), &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool SimpleRenderer::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                    VkMemoryPropertyFlags props,
                                    VkBuffer& buf, VkDeviceMemory& mem) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size  = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &info, nullptr, &buf) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, buf, &req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = find_memory_type(req.memoryTypeBits, props);
    if (alloc.memoryTypeIndex == UINT32_MAX) return false;

    if (vkAllocateMemory(device_, &alloc, nullptr, &mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(device_, buf, mem, 0);
    return true;
}

bool SimpleRenderer::create_mesh_buffers() {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    generate_icosphere(vertices, indices);
    index_count_ = static_cast<uint32_t>(indices.size());

    VkMemoryPropertyFlags host_visible =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    // Vertex buffer
    VkDeviceSize vb_size = vertices.size() * sizeof(Vertex);
    if (!create_buffer(vb_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, host_visible,
                       vertex_buffer_, vertex_memory_))
        return false;

    void* mapped = nullptr;
    vkMapMemory(device_, vertex_memory_, 0, vb_size, 0, &mapped);
    memcpy(mapped, vertices.data(), vb_size);
    vkUnmapMemory(device_, vertex_memory_);

    // Index buffer
    VkDeviceSize ib_size = indices.size() * sizeof(uint32_t);
    if (!create_buffer(ib_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, host_visible,
                       index_buffer_, index_memory_))
        return false;

    vkMapMemory(device_, index_memory_, 0, ib_size, 0, &mapped);
    memcpy(mapped, indices.data(), ib_size);
    vkUnmapMemory(device_, index_memory_);

    return true;
}

bool SimpleRenderer::create_instance_buffer() {
    instance_buffer_size_ = MAX_INSTANCES * 4 * sizeof(float);
    VkMemoryPropertyFlags host_visible =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    return create_buffer(instance_buffer_size_,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         host_visible,
                         instance_buffer_, instance_memory_);
}

bool SimpleRenderer::create_descriptor_sets() {
    // Layout: binding 0 = UBO, binding 1 = SSBO
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 2;
    layout_info.pBindings    = bindings;

    if (vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &desc_set_layout_) != VK_SUCCESS)
        return false;

    // Create UBO buffer
    VkMemoryPropertyFlags host_visible =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!create_buffer(sizeof(SceneUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                       host_visible, ubo_buffer_, ubo_memory_))
        return false;

    // Descriptor pool
    VkDescriptorPoolSize pool_sizes[2]{};
    pool_sizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[0].descriptorCount = 1;
    pool_sizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets       = 1;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes    = pool_sizes;

    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &desc_pool_) != VK_SUCCESS)
        return false;

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = desc_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts        = &desc_set_layout_;

    if (vkAllocateDescriptorSets(device_, &alloc, &desc_set_) != VK_SUCCESS)
        return false;

    // Write descriptors
    VkDescriptorBufferInfo ubo_info{};
    ubo_info.buffer = ubo_buffer_;
    ubo_info.offset = 0;
    ubo_info.range  = sizeof(SceneUBO);

    VkDescriptorBufferInfo ssbo_info{};
    ssbo_info.buffer = instance_buffer_;
    ssbo_info.offset = 0;
    ssbo_info.range  = instance_buffer_size_;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = desc_set_;
    writes[0].dstBinding      = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo     = &ubo_info;
    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = desc_set_;
    writes[1].dstBinding      = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo     = &ssbo_info;

    vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    return true;
}

} // namespace pictor

#endif // PICTOR_HAS_VULKAN
