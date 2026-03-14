/// Pictor Graphics Demo — PBR + Shadow + GI
///
/// Demonstrates:
///   - PBR metallic cube (high metalness Cook-Torrance BRDF)
///   - Directional light with CSM shadow mapping
///   - Spotlight with cone attenuation
///   - GI system integration (SSAO + ambient hemisphere lighting)
///   - Ground plane for shadow reception
///   - Orbiting camera
///
/// Build target: pictor_graphics_demo (separate from pictor_demo)

#include "pictor/pictor.h"
#include "pictor/surface/vulkan_context.h"
#include "pictor/surface/glfw_surface_provider.h"
#include <GLFW/glfw3.h>

#include <cstdio>
#include <cmath>
#include <cstring>
#include <chrono>
#include <vector>
#include <string>

using namespace pictor;

// ============================================================
// Math Helpers
// ============================================================

namespace {

void mat4_identity(float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void mat4_look_at(float* out, const float* eye, const float* center, const float* up) {
    float fx = center[0] - eye[0], fy = center[1] - eye[1], fz = center[2] - eye[2];
    float fl = std::sqrt(fx*fx + fy*fy + fz*fz);
    fx /= fl; fy /= fl; fz /= fl;

    float sx = fy*up[2] - fz*up[1], sy = fz*up[0] - fx*up[2], sz = fx*up[1] - fy*up[0];
    float sl = std::sqrt(sx*sx + sy*sy + sz*sz);
    sx /= sl; sy /= sl; sz /= sl;

    float ux = sy*fz - sz*fy, uy = sz*fx - sx*fz, uz = sx*fy - sy*fx;

    mat4_identity(out);
    out[0] = sx;  out[4] = sy;  out[8]  = sz;  out[12] = -(sx*eye[0]+sy*eye[1]+sz*eye[2]);
    out[1] = ux;  out[5] = uy;  out[9]  = uz;  out[13] = -(ux*eye[0]+uy*eye[1]+uz*eye[2]);
    out[2] = -fx; out[6] = -fy; out[10] = -fz; out[14] = (fx*eye[0]+fy*eye[1]+fz*eye[2]);
    out[3] = 0;   out[7] = 0;   out[11] = 0;   out[15] = 1.0f;
}

void mat4_perspective(float* out, float fovy_rad, float aspect, float near_z, float far_z) {
    memset(out, 0, 16 * sizeof(float));
    float f = 1.0f / std::tan(fovy_rad * 0.5f);
    out[0]  = f / aspect;
    out[5]  = -f; // Vulkan Y-flip
    out[10] = far_z / (near_z - far_z);
    out[11] = -1.0f;
    out[14] = (near_z * far_z) / (near_z - far_z);
}

void mat4_multiply(float* out, const float* a, const float* b) {
    float tmp[16];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[j * 4 + i] = 0.0f;
            for (int k = 0; k < 4; k++) {
                tmp[j * 4 + i] += a[k * 4 + i] * b[j * 4 + k];
            }
        }
    }
    memcpy(out, tmp, 16 * sizeof(float));
}

void mat4_scale(float* out, float sx, float sy, float sz) {
    mat4_identity(out);
    out[0] = sx; out[5] = sy; out[10] = sz;
}

void mat4_translation(float* out, float tx, float ty, float tz) {
    mat4_identity(out);
    out[12] = tx; out[13] = ty; out[14] = tz;
}

void mat4_rotation_y(float* out, float angle_rad) {
    mat4_identity(out);
    float c = std::cos(angle_rad);
    float s = std::sin(angle_rad);
    out[0]  =  c; out[8]  = s;
    out[2]  = -s; out[10] = c;
}

} // anonymous namespace

// ============================================================
// Scene UBO (must match pbr_demo.vert / pbr_demo.frag)
// ============================================================

struct SceneUBO {
    float view[16];
    float proj[16];
    float viewProj[16];
    float cameraPos[4];
    float ambientColor[4];

    float sunDirection[4];
    float sunColor[4];

    float spotPosition[4];
    float spotDirection[4];
    float spotColor[4];
    float spotParams[4];

    float shadowViewProj[16];
    float shadowParams[4];

    float time;
    float pad0, pad1, pad2;
};

// Per-instance data (must match shader InstanceData struct)
struct InstanceData {
    float model[16];
    float baseColor[4];
    float pbrParams[4];     // metallic, roughness, ao, emissiveStrength
    float emissiveColor[4];
};

// ============================================================
// Mesh Generation
// ============================================================

struct Vertex {
    float pos[3];
    float normal[3];
};

static void generate_cube(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices) {
    // 6 faces, 4 vertices each, unique normals per face
    const float P = 0.5f;
    const float N = -0.5f;

    // Face data: position + normal
    struct FaceVert { float x, y, z, nx, ny, nz; };
    const FaceVert face_data[] = {
        // +Z face (front)
        {N, N, P, 0, 0, 1}, {P, N, P, 0, 0, 1}, {P, P, P, 0, 0, 1}, {N, P, P, 0, 0, 1},
        // -Z face (back)
        {P, N, N, 0, 0,-1}, {N, N, N, 0, 0,-1}, {N, P, N, 0, 0,-1}, {P, P, N, 0, 0,-1},
        // +X face (right)
        {P, N, P, 1, 0, 0}, {P, N, N, 1, 0, 0}, {P, P, N, 1, 0, 0}, {P, P, P, 1, 0, 0},
        // -X face (left)
        {N, N, N,-1, 0, 0}, {N, N, P,-1, 0, 0}, {N, P, P,-1, 0, 0}, {N, P, N,-1, 0, 0},
        // +Y face (top)
        {N, P, P, 0, 1, 0}, {P, P, P, 0, 1, 0}, {P, P, N, 0, 1, 0}, {N, P, N, 0, 1, 0},
        // -Y face (bottom)
        {N, N, N, 0,-1, 0}, {P, N, N, 0,-1, 0}, {P, N, P, 0,-1, 0}, {N, N, P, 0,-1, 0},
    };

    vertices.clear();
    indices.clear();

    for (int i = 0; i < 24; i++) {
        Vertex v;
        v.pos[0]    = face_data[i].x;
        v.pos[1]    = face_data[i].y;
        v.pos[2]    = face_data[i].z;
        v.normal[0] = face_data[i].nx;
        v.normal[1] = face_data[i].ny;
        v.normal[2] = face_data[i].nz;
        vertices.push_back(v);
    }

    for (int face = 0; face < 6; face++) {
        uint32_t base = face * 4;
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }
}

static void generate_ground_plane(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                                   float half_size = 10.0f) {
    vertices.clear();
    indices.clear();

    Vertex v0 = {{-half_size, 0.0f, -half_size}, {0, 1, 0}};
    Vertex v1 = {{ half_size, 0.0f, -half_size}, {0, 1, 0}};
    Vertex v2 = {{ half_size, 0.0f,  half_size}, {0, 1, 0}};
    Vertex v3 = {{-half_size, 0.0f,  half_size}, {0, 1, 0}};

    vertices.push_back(v0);
    vertices.push_back(v1);
    vertices.push_back(v2);
    vertices.push_back(v3);

    indices.push_back(0); indices.push_back(1); indices.push_back(2);
    indices.push_back(0); indices.push_back(2); indices.push_back(3);
}

/// Generate a UV sphere with given subdivisions.
static void generate_sphere(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                             float radius = 0.5f, uint32_t slices = 24, uint32_t stacks = 16) {
    vertices.clear();
    indices.clear();

    const float pi = 3.14159265358979f;

    // Vertices
    for (uint32_t j = 0; j <= stacks; ++j) {
        float phi = pi * static_cast<float>(j) / static_cast<float>(stacks);
        float sp = std::sin(phi), cp = std::cos(phi);

        for (uint32_t i = 0; i <= slices; ++i) {
            float theta = 2.0f * pi * static_cast<float>(i) / static_cast<float>(slices);
            float st = std::sin(theta), ct = std::cos(theta);

            float nx = sp * ct, ny = cp, nz = sp * st;
            Vertex v;
            v.pos[0]    = radius * nx;
            v.pos[1]    = radius * ny;
            v.pos[2]    = radius * nz;
            v.normal[0] = nx;
            v.normal[1] = ny;
            v.normal[2] = nz;
            vertices.push_back(v);
        }
    }

    // Indices
    for (uint32_t j = 0; j < stacks; ++j) {
        for (uint32_t i = 0; i < slices; ++i) {
            uint32_t a = j * (slices + 1) + i;
            uint32_t b = a + slices + 1;

            indices.push_back(a);
            indices.push_back(b);
            indices.push_back(a + 1);

            indices.push_back(a + 1);
            indices.push_back(b);
            indices.push_back(b + 1);
        }
    }
}

// ============================================================
// PBR Demo Renderer (Vulkan)
// ============================================================

#ifdef PICTOR_HAS_VULKAN

class PBRDemoRenderer {
public:
    bool initialize(VulkanContext& vk_ctx, const char* shader_dir) {
        vk_ctx_ = &vk_ctx;
        device_ = vk_ctx.device();

        if (!create_descriptor_layout()) return false;
        if (!create_pipeline(shader_dir)) return false;
        if (!create_buffers()) return false;
        if (!create_descriptor_sets()) return false;

        initialized_ = true;
        return true;
    }

    void shutdown() {
        if (!initialized_) return;
        vkDeviceWaitIdle(device_);

        if (pipeline_)        vkDestroyPipeline(device_, pipeline_, nullptr);
        if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        if (desc_set_layout_) vkDestroyDescriptorSetLayout(device_, desc_set_layout_, nullptr);
        if (desc_pool_)       vkDestroyDescriptorPool(device_, desc_pool_, nullptr);

        auto destroy_buf = [this](VkBuffer& b, VkDeviceMemory& m) {
            if (b) vkDestroyBuffer(device_, b, nullptr);
            if (m) vkFreeMemory(device_, m, nullptr);
            b = VK_NULL_HANDLE; m = VK_NULL_HANDLE;
        };

        destroy_buf(ubo_buffer_, ubo_memory_);
        destroy_buf(instance_buffer_, instance_memory_);
        destroy_buf(cube_vb_, cube_vb_mem_);
        destroy_buf(cube_ib_, cube_ib_mem_);
        destroy_buf(floor_vb_, floor_vb_mem_);
        destroy_buf(floor_ib_, floor_ib_mem_);
        destroy_buf(sphere_vb_, sphere_vb_mem_);
        destroy_buf(sphere_ib_, sphere_ib_mem_);

        initialized_ = false;
    }

    ~PBRDemoRenderer() { shutdown(); }

    void update_scene(const SceneUBO& ubo,
                      const InstanceData* instances, uint32_t instance_count) {
        // Upload UBO
        void* mapped = nullptr;
        vkMapMemory(device_, ubo_memory_, 0, sizeof(SceneUBO), 0, &mapped);
        memcpy(mapped, &ubo, sizeof(SceneUBO));
        vkUnmapMemory(device_, ubo_memory_);

        // Upload instance data
        instance_count_ = instance_count;
        VkDeviceSize inst_size = instance_count * sizeof(InstanceData);
        vkMapMemory(device_, instance_memory_, 0, inst_size, 0, &mapped);
        memcpy(mapped, instances, inst_size);
        vkUnmapMemory(device_, instance_memory_);
    }

    /// Draw call descriptor for a mesh group
    struct DrawGroup {
        enum MeshType { CUBE, FLOOR, SPHERE };
        MeshType mesh;
        uint32_t instance_start;
        uint32_t instance_count;
    };

    void render(VkCommandBuffer cmd, VkRenderPass render_pass,
                VkFramebuffer framebuffer, VkExtent2D extent,
                const DrawGroup* groups, uint32_t group_count) {

        VkClearValue clear_values[2];
        clear_values[0].color = {{0.02f, 0.02f, 0.04f, 1.0f}};
        clear_values[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rp_info{};
        rp_info.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_info.renderPass  = render_pass;
        rp_info.framebuffer = framebuffer;
        rp_info.renderArea  = {{0, 0}, extent};
        rp_info.clearValueCount = 2;
        rp_info.pClearValues    = clear_values;

        vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout_, 0, 1, &desc_set_, 0, nullptr);

        VkViewport viewport{0, 0, (float)extent.width, (float)extent.height, 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        for (uint32_t g = 0; g < group_count; ++g) {
            if (groups[g].instance_count == 0) continue;

            VkBuffer vb = VK_NULL_HANDLE;
            VkBuffer ib = VK_NULL_HANDLE;
            uint32_t idx_count = 0;

            switch (groups[g].mesh) {
                case DrawGroup::CUBE:
                    vb = cube_vb_; ib = cube_ib_; idx_count = cube_index_count_; break;
                case DrawGroup::FLOOR:
                    vb = floor_vb_; ib = floor_ib_; idx_count = floor_index_count_; break;
                case DrawGroup::SPHERE:
                    vb = sphere_vb_; ib = sphere_ib_; idx_count = sphere_index_count_; break;
            }

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
            vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, idx_count, groups[g].instance_count,
                             0, 0, groups[g].instance_start);
        }

        vkCmdEndRenderPass(cmd);
    }

private:
    // Vulkan buffer creation helper
    bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags props, VkBuffer& buf, VkDeviceMemory& mem) {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size  = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device_, &info, nullptr, &buf) != VK_SUCCESS) return false;

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, buf, &req);

        VkMemoryAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize  = req.size;
        alloc_info.memoryTypeIndex = find_memory_type(req.memoryTypeBits, props);

        if (vkAllocateMemory(device_, &alloc_info, nullptr, &mem) != VK_SUCCESS) return false;
        vkBindBufferMemory(device_, buf, mem, 0);
        return true;
    }

    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props) {
        VkPhysicalDeviceMemoryProperties mem_props;
        vkGetPhysicalDeviceMemoryProperties(vk_ctx_->physical_device(), &mem_props);
        for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
            if ((type_filter & (1 << i)) &&
                (mem_props.memoryTypes[i].propertyFlags & props) == props) {
                return i;
            }
        }
        return 0;
    }

    void upload_buffer(VkBuffer buf, VkDeviceMemory mem, const void* data, VkDeviceSize size) {
        void* mapped = nullptr;
        vkMapMemory(device_, mem, 0, size, 0, &mapped);
        memcpy(mapped, data, size);
        vkUnmapMemory(device_, mem);
    }

    VkShaderModule load_shader(const char* path) {
        FILE* f = fopen(path, "rb");
        if (!f) return VK_NULL_HANDLE;
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<char> code(len);
        fread(code.data(), 1, len, f);
        fclose(f);

        VkShaderModuleCreateInfo info{};
        info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = code.size();
        info.pCode    = reinterpret_cast<const uint32_t*>(code.data());

        VkShaderModule mod;
        if (vkCreateShaderModule(device_, &info, nullptr, &mod) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        return mod;
    }

    bool create_descriptor_layout() {
        VkDescriptorSetLayoutBinding bindings[2] = {};

        // binding 0: UBO (scene params)
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        // binding 1: SSBO (instance data)
        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 2;
        layout_info.pBindings    = bindings;

        return vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &desc_set_layout_) == VK_SUCCESS;
    }

    bool create_pipeline(const char* shader_dir) {
        std::string vert_path = std::string(shader_dir) + "/pbr_demo.vert.spv";
        std::string frag_path = std::string(shader_dir) + "/pbr_demo.frag.spv";

        VkShaderModule vert_mod = load_shader(vert_path.c_str());
        VkShaderModule frag_mod = load_shader(frag_path.c_str());

        if (!vert_mod || !frag_mod) {
            fprintf(stderr, "PBRDemoRenderer: failed to load shaders from %s\n", shader_dir);
            if (vert_mod) vkDestroyShaderModule(device_, vert_mod, nullptr);
            if (frag_mod) vkDestroyShaderModule(device_, frag_mod, nullptr);
            return false;
        }

        // Pipeline layout
        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts    = &desc_set_layout_;

        if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
            vkDestroyShaderModule(device_, vert_mod, nullptr);
            vkDestroyShaderModule(device_, frag_mod, nullptr);
            return false;
        }

        // Shader stages
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert_mod;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag_mod;
        stages[1].pName  = "main";

        // Vertex input: pos(vec3) + normal(vec3)
        VkVertexInputBindingDescription bind_desc{};
        bind_desc.binding   = 0;
        bind_desc.stride    = sizeof(Vertex);
        bind_desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attr_descs[2] = {};
        attr_descs[0].location = 0;
        attr_descs[0].binding  = 0;
        attr_descs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
        attr_descs[0].offset   = offsetof(Vertex, pos);
        attr_descs[1].location = 1;
        attr_descs[1].binding  = 0;
        attr_descs[1].format   = VK_FORMAT_R32G32B32_SFLOAT;
        attr_descs[1].offset   = offsetof(Vertex, normal);

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount   = 1;
        vi.pVertexBindingDescriptions      = &bind_desc;
        vi.vertexAttributeDescriptionCount = 2;
        vi.pVertexAttributeDescriptions    = attr_descs;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{};
        vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode    = VK_CULL_MODE_BACK_BIT;
        rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable  = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp   = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState blend_att{};
        blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments    = &blend_att;

        VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{};
        dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates    = dyn_states;

        VkGraphicsPipelineCreateInfo pipe_info{};
        pipe_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipe_info.stageCount          = 2;
        pipe_info.pStages             = stages;
        pipe_info.pVertexInputState   = &vi;
        pipe_info.pInputAssemblyState = &ia;
        pipe_info.pViewportState      = &vp;
        pipe_info.pRasterizationState = &rs;
        pipe_info.pMultisampleState   = &ms;
        pipe_info.pDepthStencilState  = &ds;
        pipe_info.pColorBlendState    = &cb;
        pipe_info.pDynamicState       = &dyn;
        pipe_info.layout              = pipeline_layout_;
        pipe_info.renderPass          = vk_ctx_->default_render_pass();

        VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1,
                                                     &pipe_info, nullptr, &pipeline_);

        vkDestroyShaderModule(device_, vert_mod, nullptr);
        vkDestroyShaderModule(device_, frag_mod, nullptr);

        return result == VK_SUCCESS;
    }

    bool create_buffers() {
        VkMemoryPropertyFlags host_vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        // UBO
        if (!create_buffer(sizeof(SceneUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           host_vis, ubo_buffer_, ubo_memory_))
            return false;

        // Instance SSBO (max 64 instances for this demo)
        if (!create_buffer(MAX_INSTANCES * sizeof(InstanceData),
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           host_vis, instance_buffer_, instance_memory_))
            return false;

        // Cube mesh
        std::vector<Vertex> cube_verts;
        std::vector<uint32_t> cube_idxs;
        generate_cube(cube_verts, cube_idxs);
        cube_index_count_ = static_cast<uint32_t>(cube_idxs.size());

        if (!create_buffer(cube_verts.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           host_vis, cube_vb_, cube_vb_mem_))
            return false;
        upload_buffer(cube_vb_, cube_vb_mem_, cube_verts.data(), cube_verts.size() * sizeof(Vertex));

        if (!create_buffer(cube_idxs.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                           host_vis, cube_ib_, cube_ib_mem_))
            return false;
        upload_buffer(cube_ib_, cube_ib_mem_, cube_idxs.data(), cube_idxs.size() * sizeof(uint32_t));

        // Floor mesh
        std::vector<Vertex> floor_verts;
        std::vector<uint32_t> floor_idxs;
        generate_ground_plane(floor_verts, floor_idxs, 15.0f);
        floor_index_count_ = static_cast<uint32_t>(floor_idxs.size());

        if (!create_buffer(floor_verts.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           host_vis, floor_vb_, floor_vb_mem_))
            return false;
        upload_buffer(floor_vb_, floor_vb_mem_, floor_verts.data(), floor_verts.size() * sizeof(Vertex));

        if (!create_buffer(floor_idxs.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                           host_vis, floor_ib_, floor_ib_mem_))
            return false;
        upload_buffer(floor_ib_, floor_ib_mem_, floor_idxs.data(), floor_idxs.size() * sizeof(uint32_t));

        // Sphere mesh
        std::vector<Vertex> sphere_verts;
        std::vector<uint32_t> sphere_idxs;
        generate_sphere(sphere_verts, sphere_idxs, 0.5f, 32, 24);
        sphere_index_count_ = static_cast<uint32_t>(sphere_idxs.size());

        if (!create_buffer(sphere_verts.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           host_vis, sphere_vb_, sphere_vb_mem_))
            return false;
        upload_buffer(sphere_vb_, sphere_vb_mem_, sphere_verts.data(), sphere_verts.size() * sizeof(Vertex));

        if (!create_buffer(sphere_idxs.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                           host_vis, sphere_ib_, sphere_ib_mem_))
            return false;
        upload_buffer(sphere_ib_, sphere_ib_mem_, sphere_idxs.data(), sphere_idxs.size() * sizeof(uint32_t));

        return true;
    }

    bool create_descriptor_sets() {
        VkDescriptorPoolSize pool_sizes[2] = {};
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

        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool     = desc_pool_;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts        = &desc_set_layout_;

        if (vkAllocateDescriptorSets(device_, &alloc_info, &desc_set_) != VK_SUCCESS)
            return false;

        VkDescriptorBufferInfo ubo_info{ubo_buffer_, 0, sizeof(SceneUBO)};
        VkDescriptorBufferInfo ssbo_info{instance_buffer_, 0, MAX_INSTANCES * sizeof(InstanceData)};

        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = desc_set_;
        writes[0].dstBinding      = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo     = &ubo_info;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = desc_set_;
        writes[1].dstBinding      = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo     = &ssbo_info;

        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
        return true;
    }

    VulkanContext* vk_ctx_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    bool initialized_ = false;

    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet desc_set_ = VK_NULL_HANDLE;

    VkBuffer ubo_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory ubo_memory_ = VK_NULL_HANDLE;
    VkBuffer instance_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory instance_memory_ = VK_NULL_HANDLE;

    VkBuffer cube_vb_ = VK_NULL_HANDLE, cube_ib_ = VK_NULL_HANDLE;
    VkDeviceMemory cube_vb_mem_ = VK_NULL_HANDLE, cube_ib_mem_ = VK_NULL_HANDLE;
    VkBuffer floor_vb_ = VK_NULL_HANDLE, floor_ib_ = VK_NULL_HANDLE;
    VkDeviceMemory floor_vb_mem_ = VK_NULL_HANDLE, floor_ib_mem_ = VK_NULL_HANDLE;
    VkBuffer sphere_vb_ = VK_NULL_HANDLE, sphere_ib_ = VK_NULL_HANDLE;
    VkDeviceMemory sphere_vb_mem_ = VK_NULL_HANDLE, sphere_ib_mem_ = VK_NULL_HANDLE;

    uint32_t cube_index_count_ = 0;
    uint32_t floor_index_count_ = 0;
    uint32_t sphere_index_count_ = 0;
    uint32_t instance_count_ = 0;

    static constexpr uint32_t MAX_INSTANCES = 64;
};

#endif // PICTOR_HAS_VULKAN

// ============================================================
// Main
// ============================================================

int main() {
    printf("=== Pictor Graphics Demo — PBR + Shadow + GI ===\n\n");

    // ---- 1. GLFW Window ----
    GlfwSurfaceProvider surface_provider;
    GlfwWindowConfig win_cfg;
    win_cfg.width  = 1920;
    win_cfg.height = 1080;
    win_cfg.title  = "Pictor — PBR Graphics Demo";
    win_cfg.vsync  = true;

    if (!surface_provider.create(win_cfg)) {
        fprintf(stderr, "Failed to create GLFW window\n");
        return 1;
    }

    // ---- 2. Vulkan Context ----
    VulkanContext vk_ctx;
    VulkanContextConfig vk_cfg;
    vk_cfg.app_name   = "Pictor Graphics Demo";
    vk_cfg.validation = true;

    if (!vk_ctx.initialize(&surface_provider, vk_cfg)) {
        fprintf(stderr, "Failed to initialize Vulkan context\n");
        surface_provider.destroy();
        return 1;
    }

    uint32_t screen_w = 1920, screen_h = 1080;
#ifdef PICTOR_HAS_VULKAN
    screen_w = vk_ctx.swapchain_extent().width;
    screen_h = vk_ctx.swapchain_extent().height;
#endif
    printf("Vulkan initialized: %ux%u\n", screen_w, screen_h);

    // ---- 3. PBR Demo Renderer ----
#ifdef PICTOR_HAS_VULKAN
    PBRDemoRenderer pbr_renderer;
    std::string shader_dir = "shaders";
    if (!pbr_renderer.initialize(vk_ctx, shader_dir.c_str())) {
        // Try relative to executable
        shader_dir = "../shaders";
        if (!pbr_renderer.initialize(vk_ctx, shader_dir.c_str())) {
            fprintf(stderr, "Failed to initialize PBR renderer\n");
            vk_ctx.shutdown();
            surface_provider.destroy();
            return 1;
        }
    }
    printf("PBR Demo Renderer initialized.\n");
#endif

    // ---- 4. Pictor Renderer (data pipeline) ----
    RendererConfig pictor_cfg;
    pictor_cfg.initial_profile = "Standard";
    pictor_cfg.screen_width    = screen_w;
    pictor_cfg.screen_height   = screen_h;
    pictor_cfg.profiler_enabled = true;
    pictor_cfg.overlay_mode    = OverlayMode::STANDARD;

    PictorRenderer renderer;
    renderer.initialize(pictor_cfg);

    // ---- 5. Configure GI System ----
    GIConfig gi_cfg;
    gi_cfg.shadow_enabled   = true;
    gi_cfg.ssao_enabled     = true;
    gi_cfg.gi_probes_enabled = false;

    // Shadow: PCSS for contact-hardening soft shadows
    gi_cfg.shadow.cascade_count    = 3;
    gi_cfg.shadow.resolution       = 2048;
    gi_cfg.shadow.filter_mode      = ShadowFilterMode::PCSS;
    gi_cfg.shadow.shadow_strength  = 0.85f;
    gi_cfg.shadow.pcss_light_size  = 0.06f;
    gi_cfg.shadow.max_shadow_dist  = 50.0f;

    // SSAO
    gi_cfg.ssao.sample_count  = 32;
    gi_cfg.ssao.radius        = 0.5f;
    gi_cfg.ssao.intensity     = 1.2f;
    gi_cfg.ssao.blur_enabled  = true;

    renderer.set_gi_config(gi_cfg);

    // Directional light (sun)
    DirectionalLight sun;
    sun.direction = {0.5f, -0.8f, 0.3f};
    sun.color     = {1.0f, 0.95f, 0.85f};
    sun.intensity = 2.5f;
    renderer.set_directional_light(sun);

    // ---- 6. Build PBR Materials via Builder ----

    // Material 0: Metallic cube (high metalness, low roughness — chrome)
    auto metal_cube_mat = BaseMaterialBuilder()
        .base_color(0.95f, 0.93f, 0.88f)
        .metallic_value(0.95f)
        .roughness_value(0.15f)
        .ao_strength(1.0f)
        .enable_cast_shadow(true)
        .enable_receive_shadow(true)
        .build(0);

    // Material 1: Ground plane (non-metallic concrete)
    auto floor_mat = BaseMaterialBuilder()
        .base_color(0.3f, 0.3f, 0.32f)
        .metallic_value(0.0f)
        .roughness_value(0.8f)
        .ao_strength(1.0f)
        .enable_cast_shadow(false)
        .enable_receive_shadow(true)
        .build(1);

    // Material 2: Copper metallic sphere
    auto copper_mat = BaseMaterialBuilder()
        .base_color(0.95f, 0.64f, 0.54f)
        .metallic_value(0.9f)
        .roughness_value(0.25f)
        .ao_strength(1.0f)
        .enable_cast_shadow(true)
        .enable_receive_shadow(true)
        .build(2);

    // Material 3: Brushed gold sphere
    auto gold_mat = BaseMaterialBuilder()
        .base_color(1.0f, 0.84f, 0.0f)
        .metallic_value(0.95f)
        .roughness_value(0.35f)
        .ao_strength(1.0f)
        .enable_cast_shadow(true)
        .enable_receive_shadow(true)
        .build(3);

    // Material 4: Polished titanium sphere
    auto titanium_mat = BaseMaterialBuilder()
        .base_color(0.54f, 0.57f, 0.58f)
        .metallic_value(0.85f)
        .roughness_value(0.2f)
        .ao_strength(1.0f)
        .enable_cast_shadow(true)
        .enable_receive_shadow(true)
        .build(4);

    // Material 5: Dynamic sphere (emissive teal)
    auto dynamic_mat = BaseMaterialBuilder()
        .base_color(0.15f, 0.15f, 0.15f)
        .metallic_value(0.7f)
        .roughness_value(0.3f)
        .emissive_color(0.1f, 0.8f, 0.6f)
        .enable_cast_shadow(true)
        .enable_receive_shadow(true)
        .build(5);

    // Materials are built to demonstrate the BaseMaterialBuilder API.
    // In a full renderer these would be registered with a MaterialRegistry.
    (void)metal_cube_mat; (void)floor_mat;  (void)copper_mat;
    (void)gold_mat;       (void)titanium_mat; (void)dynamic_mat;

    printf("PBR materials built: 6 materials\n");

    // ---- 7. Register Objects with Pictor ----
    // Mesh IDs: 0 = cube, 1 = floor, 2 = sphere

    // Central metallic cube (STATIC)
    {
        ObjectDescriptor desc;
        desc.mesh      = 0;
        desc.material  = 0;
        desc.transform = float4x4::identity();
        desc.transform.set_translation(0.0f, 1.5f, 0.0f);
        desc.bounds.min = {-1.5f, 0.0f, -1.5f};
        desc.bounds.max = { 1.5f, 3.0f,  1.5f};
        desc.flags = ObjectFlags::STATIC | ObjectFlags::CAST_SHADOW | ObjectFlags::RECEIVE_SHADOW;
        renderer.register_object(desc);
    }

    // Ground plane (STATIC)
    {
        ObjectDescriptor desc;
        desc.mesh      = 1;
        desc.material  = 1;
        desc.transform = float4x4::identity();
        desc.bounds.min = {-15.0f, -0.01f, -15.0f};
        desc.bounds.max = { 15.0f,  0.01f,  15.0f};
        desc.flags = ObjectFlags::STATIC | ObjectFlags::RECEIVE_SHADOW;
        renderer.register_object(desc);
    }

    // Static sphere 1: Copper (right front)
    {
        ObjectDescriptor desc;
        desc.mesh      = 2;
        desc.material  = 2;
        desc.transform = float4x4::identity();
        desc.transform.set_translation(4.0f, 0.8f, 2.0f);
        desc.bounds.min = {3.2f, 0.0f, 1.2f};
        desc.bounds.max = {4.8f, 1.6f, 2.8f};
        desc.flags = ObjectFlags::STATIC | ObjectFlags::CAST_SHADOW | ObjectFlags::RECEIVE_SHADOW;
        renderer.register_object(desc);
    }

    // Static sphere 2: Gold (left front)
    {
        ObjectDescriptor desc;
        desc.mesh      = 2;
        desc.material  = 3;
        desc.transform = float4x4::identity();
        desc.transform.set_translation(-3.5f, 0.6f, 1.5f);
        desc.bounds.min = {-4.1f, 0.0f, 0.9f};
        desc.bounds.max = {-2.9f, 1.2f, 2.1f};
        desc.flags = ObjectFlags::STATIC | ObjectFlags::CAST_SHADOW | ObjectFlags::RECEIVE_SHADOW;
        renderer.register_object(desc);
    }

    // Static sphere 3: Titanium (behind cube)
    {
        ObjectDescriptor desc;
        desc.mesh      = 2;
        desc.material  = 4;
        desc.transform = float4x4::identity();
        desc.transform.set_translation(1.5f, 1.0f, -3.5f);
        desc.bounds.min = {0.5f, 0.0f, -4.5f};
        desc.bounds.max = {2.5f, 2.0f, -2.5f};
        desc.flags = ObjectFlags::STATIC | ObjectFlags::CAST_SHADOW | ObjectFlags::RECEIVE_SHADOW;
        renderer.register_object(desc);
    }

    // Dynamic sphere: orbiting teal emissive (DYNAMIC)
    ObjectId dynamic_sphere_id;
    {
        ObjectDescriptor desc;
        desc.mesh      = 2;
        desc.material  = 5;
        desc.transform = float4x4::identity();
        desc.transform.set_translation(5.0f, 1.5f, 0.0f);
        desc.bounds.min = {4.5f, 1.0f, -0.5f};
        desc.bounds.max = {5.5f, 2.0f,  0.5f};
        desc.flags = ObjectFlags::DYNAMIC | ObjectFlags::CAST_SHADOW | ObjectFlags::RECEIVE_SHADOW;
        dynamic_sphere_id = renderer.register_object(desc);
    }

    // ---- 7.5. Bake lightmaps for static objects ----
    printf("Baking lightmaps for static objects...\n");
    {
        GIBakeResult bake_result = renderer.bake_static_gi(
            [](float progress, const char* stage) -> bool {
                printf("  Bake [%3.0f%%] %s\n", progress * 100.0f, stage);
                return true; // continue
            });
        if (bake_result.valid) {
            renderer.apply_bake(bake_result);
            printf("Lightmap bake complete: %zu static objects baked.\n",
                   bake_result.object_ids.size());
        } else {
            printf("Lightmap bake skipped (no static objects or bake not supported).\n");
        }
    }

    // Camera
    Camera camera;
    camera.position = {0.0f, 5.0f, 10.0f};
    for (int i = 0; i < 6; ++i) {
        camera.frustum.planes[i].normal   = {0, 0, 0};
        camera.frustum.planes[i].distance = 10000.0f;
    }
    camera.frustum.planes[0] = {{1, 0, 0}, 50.0f};
    camera.frustum.planes[1] = {{-1, 0, 0}, 50.0f};
    camera.frustum.planes[2] = {{0, 1, 0}, 50.0f};
    camera.frustum.planes[3] = {{0, -1, 0}, 50.0f};
    camera.frustum.planes[4] = {{0, 0, 1}, 0.1f};
    camera.frustum.planes[5] = {{0, 0, -1}, 100.0f};

    // ---- Orbit Camera State ----
    struct OrbitCamera {
        float yaw   = 0.0f;
        float pitch = 0.45f;
        float radius = 12.0f;
        float center[3] = {0.0f, 1.0f, 0.0f};
        double lastMouseX = 0.0, lastMouseY = 0.0;
        bool dragging = false;
    };
    static OrbitCamera orbit_cam;

    GLFWwindow* win = surface_provider.glfw_window();
    glfwSetMouseButtonCallback(win, [](GLFWwindow* w, int button, int action, int /*mods*/) {
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
            orbit_cam.dragging = (action == GLFW_PRESS);
            if (orbit_cam.dragging)
                glfwGetCursorPos(w, &orbit_cam.lastMouseX, &orbit_cam.lastMouseY);
        }
    });
    glfwSetCursorPosCallback(win, [](GLFWwindow*, double xpos, double ypos) {
        if (!orbit_cam.dragging) return;
        double dx = xpos - orbit_cam.lastMouseX;
        double dy = ypos - orbit_cam.lastMouseY;
        orbit_cam.lastMouseX = xpos;
        orbit_cam.lastMouseY = ypos;
        orbit_cam.yaw   -= static_cast<float>(dx) * 0.005f;
        orbit_cam.pitch += static_cast<float>(dy) * 0.005f;
        if (orbit_cam.pitch > 1.5f)  orbit_cam.pitch = 1.5f;
        if (orbit_cam.pitch < -0.2f) orbit_cam.pitch = -0.2f;
    });
    glfwSetScrollCallback(win, [](GLFWwindow*, double /*xoffset*/, double yoffset) {
        orbit_cam.radius -= static_cast<float>(yoffset) * 1.5f;
        if (orbit_cam.radius < 3.0f)  orbit_cam.radius = 3.0f;
        if (orbit_cam.radius > 50.0f) orbit_cam.radius = 50.0f;
    });

    // ---- 8. Main Loop ----
    auto start = std::chrono::high_resolution_clock::now();
    uint64_t frame_count = 0;

    printf("\nScene setup:\n");
    printf("  - Central metallic cube (chrome, metalness=0.95, roughness=0.15)\n");
    printf("  - Ground plane (concrete, shadow receiver)\n");
    printf("  - Static sphere 1: Copper (metalness=0.9, roughness=0.25)\n");
    printf("  - Static sphere 2: Gold (metalness=0.95, roughness=0.35)\n");
    printf("  - Static sphere 3: Titanium (metalness=0.85, roughness=0.2)\n");
    printf("  - Dynamic sphere: Teal emissive (circular orbit)\n");
    printf("  - Directional light (sun, PCSS shadows)\n");
    printf("  - Spotlight (warm, orbiting cone)\n");
    printf("  - GI: SSAO + hemisphere ambient + baked lightmaps\n");
    printf("  - Mouse drag: orbit camera, Scroll: zoom\n");
    printf("\nEntering main loop. Close the window to exit.\n\n");

    while (!surface_provider.should_close()) {
        surface_provider.poll_events();

        auto now = std::chrono::high_resolution_clock::now();
        float elapsed = std::chrono::duration<float>(now - start).count();

        // Compute eye position from orbit camera
        float cos_pitch = std::cos(orbit_cam.pitch);
        float eye[3] = {
            orbit_cam.center[0] + orbit_cam.radius * cos_pitch * std::sin(orbit_cam.yaw),
            orbit_cam.center[1] + orbit_cam.radius * std::sin(orbit_cam.pitch),
            orbit_cam.center[2] + orbit_cam.radius * cos_pitch * std::cos(orbit_cam.yaw)
        };
        float up[3] = {0.0f, 1.0f, 0.0f};

        float view_mat[16], proj_mat[16], view_proj[16];
        mat4_look_at(view_mat, eye, orbit_cam.center, up);

        float aspect = static_cast<float>(screen_w) / static_cast<float>(screen_h);
        mat4_perspective(proj_mat, 0.7854f, aspect, 0.1f, 200.0f);
        mat4_multiply(view_proj, proj_mat, view_mat);

        // Spotlight: orbits opposite to camera, illuminating from the side
        float spot_angle = elapsed * 0.5f + 3.14159f;
        float spot_pos[3] = {
            6.0f * std::sin(spot_angle),
            5.0f,
            6.0f * std::cos(spot_angle)
        };
        float spot_target[3] = {0.0f, 1.0f, 0.0f};
        float spot_dir[3] = {
            spot_target[0] - spot_pos[0],
            spot_target[1] - spot_pos[1],
            spot_target[2] - spot_pos[2]
        };
        float spot_dir_len = std::sqrt(spot_dir[0]*spot_dir[0] + spot_dir[1]*spot_dir[1] + spot_dir[2]*spot_dir[2]);
        spot_dir[0] /= spot_dir_len;
        spot_dir[1] /= spot_dir_len;
        spot_dir[2] /= spot_dir_len;

        // Shadow VP from directional light
        float light_view[16], shadow_vp[16];
        float light_eye[3] = {
            -sun.direction.x * 20.0f,
            -sun.direction.y * 20.0f,
            -sun.direction.z * 20.0f
        };
        float light_center[3] = {0.0f, 0.0f, 0.0f};
        mat4_look_at(light_view, light_eye, light_center, up);
        // Orthographic projection for directional light shadow
        float ortho[16];
        memset(ortho, 0, sizeof(ortho));
        float ortho_size = 20.0f;
        ortho[0]  = 1.0f / ortho_size;
        ortho[5]  = 1.0f / ortho_size;
        ortho[10] = -1.0f / (50.0f - 0.1f);
        ortho[14] = -0.1f / (50.0f - 0.1f);
        ortho[15] = 1.0f;
        mat4_multiply(shadow_vp, ortho, light_view);

        // Build SceneUBO
        SceneUBO ubo;
        memcpy(ubo.view,    view_mat, sizeof(view_mat));
        memcpy(ubo.proj,    proj_mat, sizeof(proj_mat));
        memcpy(ubo.viewProj, view_proj, sizeof(view_proj));
        ubo.cameraPos[0] = eye[0]; ubo.cameraPos[1] = eye[1];
        ubo.cameraPos[2] = eye[2]; ubo.cameraPos[3] = 1.0f;

        ubo.ambientColor[0] = 0.15f; ubo.ambientColor[1] = 0.17f;
        ubo.ambientColor[2] = 0.25f; ubo.ambientColor[3] = 1.0f;

        // Normalize sun direction (toward light)
        float sun_len = std::sqrt(sun.direction.x*sun.direction.x +
                                   sun.direction.y*sun.direction.y +
                                   sun.direction.z*sun.direction.z);
        ubo.sunDirection[0] = -sun.direction.x / sun_len;
        ubo.sunDirection[1] = -sun.direction.y / sun_len;
        ubo.sunDirection[2] = -sun.direction.z / sun_len;
        ubo.sunDirection[3] = 0.0f;
        ubo.sunColor[0] = sun.color.x; ubo.sunColor[1] = sun.color.y;
        ubo.sunColor[2] = sun.color.z; ubo.sunColor[3] = sun.intensity;

        ubo.spotPosition[0] = spot_pos[0]; ubo.spotPosition[1] = spot_pos[1];
        ubo.spotPosition[2] = spot_pos[2]; ubo.spotPosition[3] = 0.0f;
        ubo.spotDirection[0] = spot_dir[0]; ubo.spotDirection[1] = spot_dir[1];
        ubo.spotDirection[2] = spot_dir[2]; ubo.spotDirection[3] = 0.0f;
        ubo.spotColor[0] = 1.0f; ubo.spotColor[1] = 0.85f;
        ubo.spotColor[2] = 0.6f; ubo.spotColor[3] = 4.0f; // warm, bright
        ubo.spotParams[0] = 0.3491f;   // inner cone: 20 degrees
        ubo.spotParams[1] = 0.6109f;   // outer cone: 35 degrees
        ubo.spotParams[2] = 15.0f;     // range
        ubo.spotParams[3] = 0.0f;

        memcpy(ubo.shadowViewProj, shadow_vp, sizeof(shadow_vp));
        ubo.shadowParams[0] = 0.005f;  // depth bias
        ubo.shadowParams[1] = 0.02f;   // normal bias
        ubo.shadowParams[2] = 0.85f;   // shadow strength
        ubo.shadowParams[3] = 1.0f;    // enabled

        ubo.time = elapsed;
        ubo.pad0 = ubo.pad1 = ubo.pad2 = 0.0f;

        // Dynamic sphere circular orbit
        float orbit_radius = 5.0f;
        float orbit_speed  = 0.8f;
        float orbit_height = 1.5f;
        float dyn_x = orbit_radius * std::sin(elapsed * orbit_speed);
        float dyn_z = orbit_radius * std::cos(elapsed * orbit_speed);
        float dyn_y = orbit_height + 0.3f * std::sin(elapsed * 1.5f); // gentle bob

        // Update Pictor dynamic sphere transform
        {
            float4x4 dyn_transform = float4x4::identity();
            dyn_transform.set_translation(dyn_x, dyn_y, dyn_z);
            renderer.update_transform(dynamic_sphere_id, dyn_transform);
        }

        // Helper to fill an InstanceData entry
        auto fill_instance = [](InstanceData& inst, const float* model_mat,
                                float r, float g, float b,
                                float metallic, float roughness, float ao,
                                float em_str, float em_r, float em_g, float em_b) {
            memcpy(inst.model, model_mat, sizeof(float) * 16);
            inst.baseColor[0] = r; inst.baseColor[1] = g;
            inst.baseColor[2] = b; inst.baseColor[3] = 1.0f;
            inst.pbrParams[0] = metallic; inst.pbrParams[1] = roughness;
            inst.pbrParams[2] = ao;       inst.pbrParams[3] = em_str;
            inst.emissiveColor[0] = em_r; inst.emissiveColor[1] = em_g;
            inst.emissiveColor[2] = em_b; inst.emissiveColor[3] = 0.0f;
        };

        // Build instance data: 6 objects total
        // [0] = cube (central metallic), drawn with cube mesh
        // [1] = floor, drawn with floor mesh
        // [2] = copper sphere (static), drawn with sphere mesh
        // [3] = gold sphere (static), drawn with sphere mesh
        // [4] = titanium sphere (static), drawn with sphere mesh
        // [5] = dynamic sphere (orbiting), drawn with sphere mesh
        InstanceData instances[6];

        // Instance 0: Central metallic cube
        {
            float trans[16], scale[16], rot[16], tmp[16];
            mat4_translation(trans, 0.0f, 1.5f, 0.0f);
            mat4_scale(scale, 1.5f, 1.5f, 1.5f);
            mat4_rotation_y(rot, elapsed * 0.2f);
            mat4_multiply(tmp, trans, rot);
            mat4_multiply(instances[0].model, tmp, scale);
            fill_instance(instances[0], instances[0].model,
                          0.95f, 0.93f, 0.88f,   // chrome silver
                          0.95f, 0.15f, 1.0f,     // metallic, roughness, ao
                          0.0f, 0.0f, 0.0f, 0.0f);
        }

        // Instance 1: Ground plane
        {
            float model[16];
            mat4_identity(model);
            fill_instance(instances[1], model,
                          0.3f, 0.3f, 0.32f,      // concrete gray
                          0.0f, 0.8f, 1.0f,
                          0.0f, 0.0f, 0.0f, 0.0f);
        }

        // Instance 2: Copper sphere (static, right front)
        {
            float trans[16], scale[16], model[16];
            mat4_translation(trans, 4.0f, 0.8f, 2.0f);
            mat4_scale(scale, 1.6f, 1.6f, 1.6f);
            mat4_multiply(model, trans, scale);
            fill_instance(instances[2], model,
                          0.95f, 0.64f, 0.54f,    // copper
                          0.9f, 0.25f, 1.0f,
                          0.0f, 0.0f, 0.0f, 0.0f);
        }

        // Instance 3: Gold sphere (static, left front)
        {
            float trans[16], scale[16], model[16];
            mat4_translation(trans, -3.5f, 0.6f, 1.5f);
            mat4_scale(scale, 1.2f, 1.2f, 1.2f);
            mat4_multiply(model, trans, scale);
            fill_instance(instances[3], model,
                          1.0f, 0.84f, 0.0f,      // gold
                          0.95f, 0.35f, 1.0f,
                          0.0f, 0.0f, 0.0f, 0.0f);
        }

        // Instance 4: Titanium sphere (static, behind cube)
        {
            float trans[16], scale[16], model[16];
            mat4_translation(trans, 1.5f, 1.0f, -3.5f);
            mat4_scale(scale, 2.0f, 2.0f, 2.0f);
            mat4_multiply(model, trans, scale);
            fill_instance(instances[4], model,
                          0.54f, 0.57f, 0.58f,    // titanium
                          0.85f, 0.2f, 1.0f,
                          0.0f, 0.0f, 0.0f, 0.0f);
        }

        // Instance 5: Dynamic teal sphere (orbiting)
        {
            float trans[16], scale[16], model[16];
            mat4_translation(trans, dyn_x, dyn_y, dyn_z);
            mat4_scale(scale, 1.0f, 1.0f, 1.0f);
            mat4_multiply(model, trans, scale);
            float glow = 0.5f + 0.5f * std::sin(elapsed * 2.0f);
            fill_instance(instances[5], model,
                          0.15f, 0.15f, 0.15f,
                          0.7f, 0.3f, 1.0f,
                          2.0f * glow,
                          0.1f * glow, 0.8f * glow, 0.6f * glow);
        }

#ifdef PICTOR_HAS_VULKAN
        pbr_renderer.update_scene(ubo, instances, 6);

        uint32_t image_idx = vk_ctx.acquire_next_image();
        if (image_idx == UINT32_MAX) continue;

        auto cmd = vk_ctx.command_buffers()[image_idx];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &begin_info);

        // Draw groups: cube(0), floor(1), spheres(2-5)
        PBRDemoRenderer::DrawGroup groups[3] = {
            {PBRDemoRenderer::DrawGroup::CUBE,   0, 1},  // instance 0: cube
            {PBRDemoRenderer::DrawGroup::FLOOR,  1, 1},  // instance 1: floor
            {PBRDemoRenderer::DrawGroup::SPHERE, 2, 4},  // instances 2-5: spheres
        };

        pbr_renderer.render(cmd, vk_ctx.default_render_pass(),
                            vk_ctx.framebuffers()[image_idx],
                            vk_ctx.swapchain_extent(),
                            groups, 3);

        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphore wait_sem  = vk_ctx.image_available_semaphore();
        VkSemaphore sig_sem   = vk_ctx.render_finished_semaphore();

        VkSubmitInfo submit_info{};
        submit_info.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount   = 1;
        submit_info.pWaitSemaphores      = &wait_sem;
        submit_info.pWaitDstStageMask    = &wait_stage;
        submit_info.commandBufferCount   = 1;
        submit_info.pCommandBuffers      = &cmd;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores    = &sig_sem;

        vkQueueSubmit(vk_ctx.graphics_queue(), 1, &submit_info, vk_ctx.in_flight_fence());
        vk_ctx.present(image_idx);
#endif

        // Run Pictor data pipeline
        float dt = 1.0f / 60.0f;
        camera.position = {eye[0], eye[1], eye[2]};
        renderer.begin_frame(dt);
        renderer.render(camera);
        renderer.end_frame();

        ++frame_count;

        // Print stats periodically
        if (frame_count % 120 == 0) {
            const auto& stats = renderer.get_frame_stats();
            printf("[Frame %llu] FPS: %.1f  Visible: %u  Batches: %u  "
                   "Shadow casters: GI=%s  SSAO=%s\n",
                   static_cast<unsigned long long>(frame_count),
                   stats.fps,
                   stats.visible_objects,
                   stats.batch_count,
                   gi_cfg.shadow_enabled ? "ON" : "OFF",
                   gi_cfg.ssao_enabled ? "ON" : "OFF");
        }
    }

    // ---- 9. Cleanup ----
    vk_ctx.device_wait_idle();
#ifdef PICTOR_HAS_VULKAN
    pbr_renderer.shutdown();
#endif
    renderer.shutdown();
    vk_ctx.shutdown();
    surface_provider.destroy();

    printf("\nGraphics demo finished. %llu frames rendered.\n",
           static_cast<unsigned long long>(frame_count));
    return 0;
}
