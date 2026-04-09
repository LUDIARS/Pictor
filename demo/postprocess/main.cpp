/// Pictor Post-Process Demo
///
/// Demonstrates the post-processing pipeline with:
///   - HDR rendering with bright emissive objects (for bloom)
///   - Bloom: glowing light sources with configurable threshold
///   - Depth of Field: foreground/background blur with bokeh
///   - Tone Mapping: ACES, Reinhard, Uncharted2 operator comparison
///   - Gaussian Blur: configurable blur intensity
///   - Real-time effect toggling and parameter adjustment
///
/// Controls:
///   Mouse drag   — Orbit camera
///   Scroll       — Zoom in/out
///   1            — Toggle Bloom
///   2            — Toggle Depth of Field
///   3            — Toggle Gaussian Blur
///   4            — Cycle Tone Map operator (ACES → Reinhard → Uncharted2 → ...)
///   +/-          — Adjust exposure
///   B/N          — Adjust bloom threshold
///   F/G          — Adjust DoF focus distance
///   R            — Reset all settings to defaults
///   S            — Toggle stats overlay
///
/// Build target: pictor_postprocess_demo

#include "pictor/pictor.h"
#include "pictor/surface/vulkan_context.h"
#include "pictor/surface/glfw_surface_provider.h"
#include "pictor/profiler/bitmap_text_renderer.h"
#include <GLFW/glfw3.h>

#include <cstdio>
#include <cmath>
#include <cstring>
#include <chrono>
#include <vector>
#include <string>
#include <algorithm>

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
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            tmp[j * 4 + i] = 0.0f;
            for (int k = 0; k < 4; k++)
                tmp[j * 4 + i] += a[k * 4 + i] * b[j * 4 + k];
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
    float c = std::cos(angle_rad), s = std::sin(angle_rad);
    out[0] = c; out[8] = s;
    out[2] = -s; out[10] = c;
}

const char* tonemap_operator_name(ToneMapOperator op) {
    switch (op) {
        case ToneMapOperator::ACES_FILMIC:  return "ACES Filmic";
        case ToneMapOperator::REINHARD:     return "Reinhard";
        case ToneMapOperator::REINHARD_EXT: return "Reinhard Extended";
        case ToneMapOperator::UNCHARTED2:   return "Uncharted 2 (Hable)";
        case ToneMapOperator::LINEAR_CLAMP: return "Linear (no tonemap)";
        default: return "Unknown";
    }
}

} // anonymous namespace

// ============================================================
// Scene Data Structures
// ============================================================

struct SceneUBO {
    float view[16];
    float proj[16];
    float viewProj[16];
    float cameraPos[4];
    float ambientColor[4];
    float sunDirection[4];
    float sunColor[4];
    float time;
    float dofFocusDistance;
    float dofFocusRange;
    float exposure;
};

struct InstanceData {
    float model[16];
    float baseColor[4];
    float pbrParams[4];
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
    const float P = 0.5f, N = -0.5f;
    struct FV { float x, y, z, nx, ny, nz; };
    const FV fd[] = {
        {N,N,P,0,0,1},{P,N,P,0,0,1},{P,P,P,0,0,1},{N,P,P,0,0,1},
        {P,N,N,0,0,-1},{N,N,N,0,0,-1},{N,P,N,0,0,-1},{P,P,N,0,0,-1},
        {P,N,P,1,0,0},{P,N,N,1,0,0},{P,P,N,1,0,0},{P,P,P,1,0,0},
        {N,N,N,-1,0,0},{N,N,P,-1,0,0},{N,P,P,-1,0,0},{N,P,N,-1,0,0},
        {N,P,P,0,1,0},{P,P,P,0,1,0},{P,P,N,0,1,0},{N,P,N,0,1,0},
        {N,N,N,0,-1,0},{P,N,N,0,-1,0},{P,N,P,0,-1,0},{N,N,P,0,-1,0},
    };
    vertices.clear(); indices.clear();
    for (int i = 0; i < 24; i++) {
        Vertex v; v.pos[0]=fd[i].x; v.pos[1]=fd[i].y; v.pos[2]=fd[i].z;
        v.normal[0]=fd[i].nx; v.normal[1]=fd[i].ny; v.normal[2]=fd[i].nz;
        vertices.push_back(v);
    }
    for (int f = 0; f < 6; f++) {
        uint32_t b = f * 4;
        indices.push_back(b); indices.push_back(b+1); indices.push_back(b+2);
        indices.push_back(b); indices.push_back(b+2); indices.push_back(b+3);
    }
}

static void generate_ground_plane(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                                   float half_size = 15.0f) {
    vertices.clear(); indices.clear();
    Vertex v0={{-half_size,0,-half_size},{0,1,0}}, v1={{half_size,0,-half_size},{0,1,0}};
    Vertex v2={{half_size,0,half_size},{0,1,0}}, v3={{-half_size,0,half_size},{0,1,0}};
    vertices.push_back(v0); vertices.push_back(v1);
    vertices.push_back(v2); vertices.push_back(v3);
    // CCW winding when viewed from above (+Y)
    indices.push_back(0); indices.push_back(2); indices.push_back(1);
    indices.push_back(0); indices.push_back(3); indices.push_back(2);
}

static void generate_sphere(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                              float radius = 0.5f, uint32_t slices = 32, uint32_t stacks = 24) {
    vertices.clear(); indices.clear();
    const float pi = 3.14159265f;
    for (uint32_t j = 0; j <= stacks; ++j) {
        float phi = pi * float(j) / float(stacks);
        float sp = std::sin(phi), cp = std::cos(phi);
        for (uint32_t i = 0; i <= slices; ++i) {
            float theta = 2.0f * pi * float(i) / float(slices);
            float st = std::sin(theta), ct = std::cos(theta);
            float nx = sp*ct, ny = cp, nz = sp*st;
            Vertex v; v.pos[0]=radius*nx; v.pos[1]=radius*ny; v.pos[2]=radius*nz;
            v.normal[0]=nx; v.normal[1]=ny; v.normal[2]=nz;
            vertices.push_back(v);
        }
    }
    for (uint32_t j = 0; j < stacks; ++j)
        for (uint32_t i = 0; i < slices; ++i) {
            uint32_t a = j*(slices+1)+i, b = a+slices+1;
            indices.push_back(a); indices.push_back(b); indices.push_back(a+1);
            indices.push_back(a+1); indices.push_back(b); indices.push_back(b+1);
        }
}

// ============================================================
// Scene Renderer (Vulkan draw calls for pp_demo_scene shaders)
// ============================================================

#ifdef PICTOR_HAS_VULKAN

class PPSceneRenderer {
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

    ~PPSceneRenderer() { shutdown(); }

    void update_scene(const SceneUBO& ubo,
                      const InstanceData* instances, uint32_t instance_count) {
        void* mapped = nullptr;
        vkMapMemory(device_, ubo_memory_, 0, sizeof(SceneUBO), 0, &mapped);
        memcpy(mapped, &ubo, sizeof(SceneUBO));
        vkUnmapMemory(device_, ubo_memory_);

        instance_count_ = instance_count;
        VkDeviceSize inst_size = instance_count * sizeof(InstanceData);
        vkMapMemory(device_, instance_memory_, 0, inst_size, 0, &mapped);
        memcpy(mapped, instances, inst_size);
        vkUnmapMemory(device_, instance_memory_);
    }

    struct DrawGroup {
        enum MeshType { CUBE, FLOOR, SPHERE };
        MeshType mesh;
        uint32_t instance_start;
        uint32_t instance_count;
    };

    /// Draw scene objects (must be called inside an active render pass)
    void draw(VkCommandBuffer cmd, VkExtent2D extent,
              const DrawGroup* groups, uint32_t group_count) {

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
    }

private:
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

        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 2;
        layout_info.pBindings    = bindings;

        return vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &desc_set_layout_) == VK_SUCCESS;
    }

    bool create_pipeline(const char* shader_dir) {
        std::string vert_path = std::string(shader_dir) + "/pp_demo_scene.vert.spv";
        std::string frag_path = std::string(shader_dir) + "/pp_demo_scene.frag.spv";

        VkShaderModule vert_mod = load_shader(vert_path.c_str());
        VkShaderModule frag_mod = load_shader(frag_path.c_str());

        if (!vert_mod || !frag_mod) {
            fprintf(stderr, "PPSceneRenderer: failed to load shaders from %s\n", shader_dir);
            if (vert_mod) vkDestroyShaderModule(device_, vert_mod, nullptr);
            if (frag_mod) vkDestroyShaderModule(device_, frag_mod, nullptr);
            return false;
        }

        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts    = &desc_set_layout_;

        if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
            vkDestroyShaderModule(device_, vert_mod, nullptr);
            vkDestroyShaderModule(device_, frag_mod, nullptr);
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert_mod;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag_mod;
        stages[1].pName  = "main";

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

        if (!create_buffer(sizeof(SceneUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           host_vis, ubo_buffer_, ubo_memory_))
            return false;

        if (!create_buffer(MAX_INSTANCES * sizeof(InstanceData),
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           host_vis, instance_buffer_, instance_memory_))
            return false;

        // Cube mesh
        std::vector<Vertex> verts;
        std::vector<uint32_t> idxs;
        generate_cube(verts, idxs);
        cube_index_count_ = static_cast<uint32_t>(idxs.size());

        if (!create_buffer(verts.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           host_vis, cube_vb_, cube_vb_mem_))
            return false;
        upload_buffer(cube_vb_, cube_vb_mem_, verts.data(), verts.size() * sizeof(Vertex));

        if (!create_buffer(idxs.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                           host_vis, cube_ib_, cube_ib_mem_))
            return false;
        upload_buffer(cube_ib_, cube_ib_mem_, idxs.data(), idxs.size() * sizeof(uint32_t));

        // Floor mesh
        generate_ground_plane(verts, idxs, 15.0f);
        floor_index_count_ = static_cast<uint32_t>(idxs.size());

        if (!create_buffer(verts.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           host_vis, floor_vb_, floor_vb_mem_))
            return false;
        upload_buffer(floor_vb_, floor_vb_mem_, verts.data(), verts.size() * sizeof(Vertex));

        if (!create_buffer(idxs.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                           host_vis, floor_ib_, floor_ib_mem_))
            return false;
        upload_buffer(floor_ib_, floor_ib_mem_, idxs.data(), idxs.size() * sizeof(uint32_t));

        // Sphere mesh
        generate_sphere(verts, idxs, 0.5f, 32, 24);
        sphere_index_count_ = static_cast<uint32_t>(idxs.size());

        if (!create_buffer(verts.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           host_vis, sphere_vb_, sphere_vb_mem_))
            return false;
        upload_buffer(sphere_vb_, sphere_vb_mem_, verts.data(), verts.size() * sizeof(Vertex));

        if (!create_buffer(idxs.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                           host_vis, sphere_ib_, sphere_ib_mem_))
            return false;
        upload_buffer(sphere_ib_, sphere_ib_mem_, idxs.data(), idxs.size() * sizeof(uint32_t));

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
// Post-Process Demo State
// ============================================================

struct DemoState {
    PostProcessConfig pp_config;
    int tonemap_index = 0;
    bool show_hud = true;
    float fps = 0.0f;

    // Orbit camera
    float yaw    = 0.4f;
    float pitch  = 0.35f;
    float radius = 14.0f;
    float center[3] = {0.0f, 1.5f, 0.0f};
    double lastMouseX = 0.0, lastMouseY = 0.0;
    bool dragging = false;
};
static DemoState g_state;

// ============================================================
// Main
// ============================================================

int main() {
    printf("=== Pictor Post-Process Demo ===\n\n");

    // ---- 1. GLFW Window ----
    GlfwSurfaceProvider surface_provider;
    GlfwWindowConfig win_cfg;
    win_cfg.width  = 1920;
    win_cfg.height = 1080;
    win_cfg.title  = "Pictor — Post-Process Demo (Bloom / DoF / ToneMapping / Blur)";
    win_cfg.vsync  = true;

    if (!surface_provider.create(win_cfg)) {
        fprintf(stderr, "Failed to create GLFW window\n");
        return 1;
    }

    // ---- 2. Vulkan Context ----
    VulkanContext vk_ctx;
    VulkanContextConfig vk_cfg;
    vk_cfg.app_name   = "Pictor PostProcess Demo";
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

    // ---- 3. Scene Renderer (Vulkan draw calls) ----
#ifdef PICTOR_HAS_VULKAN
    PPSceneRenderer scene_renderer;
    std::string shader_dir = "shaders";
    if (!scene_renderer.initialize(vk_ctx, shader_dir.c_str())) {
        shader_dir = "../shaders";
        if (!scene_renderer.initialize(vk_ctx, shader_dir.c_str())) {
            fprintf(stderr, "Failed to initialize scene renderer\n");
            vk_ctx.shutdown();
            surface_provider.destroy();
            return 1;
        }
    }
    printf("Scene renderer initialized.\n");

    BitmapTextRenderer text_renderer;
    if (!text_renderer.initialize(vk_ctx, shader_dir.c_str())) {
        fprintf(stderr, "Warning: Failed to initialize text renderer\n");
    }
    text_renderer.set_scale(2.0f);
#endif

    // ---- 4. Pictor Renderer ----
    RendererConfig pictor_cfg;
    pictor_cfg.initial_profile  = "Standard";
    pictor_cfg.screen_width     = screen_w;
    pictor_cfg.screen_height    = screen_h;
    pictor_cfg.profiler_enabled = true;
    pictor_cfg.overlay_mode     = OverlayMode::STANDARD;

    PictorRenderer renderer;
    renderer.initialize(pictor_cfg);

    // ---- 5. Configure Post-Process Pipeline ----
    PostProcessConfig& pp = g_state.pp_config;

    // HDR
    pp.hdr.enabled       = true;
    pp.hdr.exposure      = 1.0f;
    pp.hdr.gamma         = 2.2f;
    pp.hdr.auto_exposure = false;

    // Bloom: moderately aggressive for HDR demo
    pp.bloom.enabled        = true;
    pp.bloom.threshold      = 1.0f;
    pp.bloom.soft_threshold = 0.5f;
    pp.bloom.intensity      = 0.8f;
    pp.bloom.radius         = 5.0f;
    pp.bloom.mip_levels     = 5;
    pp.bloom.scatter        = 0.7f;

    // DoF: focus on center of scene
    pp.depth_of_field.enabled        = true;
    pp.depth_of_field.focus_distance = 12.0f;
    pp.depth_of_field.focus_range    = 4.0f;
    pp.depth_of_field.bokeh_radius   = 4.0f;
    pp.depth_of_field.near_start     = 0.0f;
    pp.depth_of_field.near_end       = 4.0f;
    pp.depth_of_field.far_start      = 18.0f;
    pp.depth_of_field.far_end        = 40.0f;
    pp.depth_of_field.sample_count   = 16;

    // Gaussian Blur: disabled by default (toggle with key)
    pp.gaussian_blur.enabled     = false;
    pp.gaussian_blur.sigma       = 2.0f;
    pp.gaussian_blur.kernel_size = 9;
    pp.gaussian_blur.separable   = true;
    pp.gaussian_blur.intensity   = 1.0f;

    // Tone Mapping: ACES Filmic (industry standard)
    pp.tone_mapping.enabled    = true;
    pp.tone_mapping.op         = ToneMapOperator::ACES_FILMIC;
    pp.tone_mapping.exposure   = 1.0f;
    pp.tone_mapping.gamma      = 2.2f;
    pp.tone_mapping.white_point = 4.0f;
    pp.tone_mapping.saturation = 1.0f;

    renderer.set_postprocess_config(pp);

    // ---- 6. Register Scene Objects ----
    // Central emissive cube (very bright — triggers bloom)
    {
        ObjectDescriptor desc;
        desc.mesh = 0; desc.material = 0;
        desc.transform = float4x4::identity();
        desc.transform.set_translation(0.0f, 2.0f, 0.0f);
        desc.bounds.min = {-1.5f, 0.5f, -1.5f};
        desc.bounds.max = { 1.5f, 3.5f,  1.5f};
        desc.flags = ObjectFlags::STATIC | ObjectFlags::CAST_SHADOW;
        renderer.register_object(desc);
    }

    // Ground plane
    {
        ObjectDescriptor desc;
        desc.mesh = 1; desc.material = 1;
        desc.transform = float4x4::identity();
        desc.bounds.min = {-15.0f, -0.01f, -15.0f};
        desc.bounds.max = { 15.0f,  0.01f,  15.0f};
        desc.flags = ObjectFlags::STATIC | ObjectFlags::RECEIVE_SHADOW;
        renderer.register_object(desc);
    }

    // Emissive light spheres (scattered around scene for bloom demo)
    struct LightSphere { float x, z, r, g, b, intensity; };
    LightSphere light_spheres[] = {
        { 5.0f,  3.0f,  1.0f, 0.2f, 0.1f, 8.0f},   // Red-orange
        {-4.0f,  5.0f,  0.1f, 0.5f, 1.0f, 6.0f},   // Cyan
        { 2.0f, -4.0f,  1.0f, 0.8f, 0.0f, 10.0f},  // Yellow (very bright!)
        {-6.0f, -2.0f,  0.6f, 0.1f, 1.0f, 5.0f},   // Purple
        { 7.0f, -1.0f,  0.0f, 1.0f, 0.3f, 7.0f},   // Green
    };

    for (int i = 0; i < 5; ++i) {
        ObjectDescriptor desc;
        desc.mesh = 2; desc.material = static_cast<MaterialHandle>(2 + i);
        desc.transform = float4x4::identity();
        desc.transform.set_translation(light_spheres[i].x, 1.0f, light_spheres[i].z);
        desc.bounds.min = {light_spheres[i].x - 0.5f, 0.5f, light_spheres[i].z - 0.5f};
        desc.bounds.max = {light_spheres[i].x + 0.5f, 1.5f, light_spheres[i].z + 0.5f};
        desc.flags = ObjectFlags::STATIC;
        renderer.register_object(desc);
    }

    // Foreground object (for DoF near blur)
    {
        ObjectDescriptor desc;
        desc.mesh = 0; desc.material = 7;
        desc.transform = float4x4::identity();
        desc.transform.set_translation(2.0f, 0.5f, 8.0f);
        desc.bounds.min = {1.5f, 0.0f, 7.5f};
        desc.bounds.max = {2.5f, 1.0f, 8.5f};
        desc.flags = ObjectFlags::STATIC | ObjectFlags::CAST_SHADOW;
        renderer.register_object(desc);
    }

    // Background objects (for DoF far blur)
    for (int i = 0; i < 3; ++i) {
        ObjectDescriptor desc;
        desc.mesh = 2; desc.material = 8;
        desc.transform = float4x4::identity();
        float bx = -5.0f + i * 5.0f;
        desc.transform.set_translation(bx, 1.2f, -10.0f);
        desc.bounds.min = {bx - 1.0f, 0.0f, -11.0f};
        desc.bounds.max = {bx + 1.0f, 2.4f, -9.0f};
        desc.flags = ObjectFlags::STATIC | ObjectFlags::CAST_SHADOW;
        renderer.register_object(desc);
    }

    // Dynamic orbiting emissive sphere
    ObjectId orbit_sphere_id;
    {
        ObjectDescriptor desc;
        desc.mesh = 2; desc.material = 9;
        desc.transform = float4x4::identity();
        desc.transform.set_translation(6.0f, 2.0f, 0.0f);
        desc.bounds.min = {5.5f, 1.5f, -0.5f};
        desc.bounds.max = {6.5f, 2.5f,  0.5f};
        desc.flags = ObjectFlags::DYNAMIC;
        orbit_sphere_id = renderer.register_object(desc);
    }

    // ---- 7. Camera ----
    Camera camera;
    camera.position = {0.0f, 5.0f, 14.0f};
    for (int i = 0; i < 6; ++i) {
        camera.frustum.planes[i].normal   = {0, 0, 0};
        camera.frustum.planes[i].distance = 10000.0f;
    }
    camera.frustum.planes[0] = {{1, 0, 0}, 50.0f};
    camera.frustum.planes[1] = {{-1, 0, 0}, 50.0f};
    camera.frustum.planes[2] = {{0, 1, 0}, 50.0f};
    camera.frustum.planes[3] = {{0, -1, 0}, 50.0f};
    camera.frustum.planes[4] = {{0, 0, 1}, 0.1f};
    camera.frustum.planes[5] = {{0, 0, -1}, 200.0f};

    // ---- 8. Input Callbacks ----
    GLFWwindow* win = surface_provider.glfw_window();

    glfwSetMouseButtonCallback(win, [](GLFWwindow* w, int button, int action, int) {
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
            g_state.dragging = (action == GLFW_PRESS);
            if (g_state.dragging)
                glfwGetCursorPos(w, &g_state.lastMouseX, &g_state.lastMouseY);
        }
    });

    glfwSetCursorPosCallback(win, [](GLFWwindow*, double xpos, double ypos) {
        if (!g_state.dragging) return;
        double dx = xpos - g_state.lastMouseX;
        double dy = ypos - g_state.lastMouseY;
        g_state.lastMouseX = xpos;
        g_state.lastMouseY = ypos;
        g_state.yaw   -= float(dx) * 0.005f;
        g_state.pitch += float(dy) * 0.005f;
        g_state.pitch = std::clamp(g_state.pitch, -0.2f, 1.5f);
    });

    glfwSetScrollCallback(win, [](GLFWwindow*, double, double yoffset) {
        g_state.radius -= float(yoffset) * 1.5f;
        g_state.radius = std::clamp(g_state.radius, 3.0f, 50.0f);
    });

    glfwSetKeyCallback(win, [](GLFWwindow*, int key, int, int action, int) {
        if (action != GLFW_PRESS) return;
        auto& pp = g_state.pp_config;

        switch (key) {
            case GLFW_KEY_1:
                pp.bloom.enabled = !pp.bloom.enabled;
                printf("[PostProcess] Bloom: %s\n", pp.bloom.enabled ? "ON" : "OFF");
                break;
            case GLFW_KEY_2:
                pp.depth_of_field.enabled = !pp.depth_of_field.enabled;
                printf("[PostProcess] DoF: %s\n", pp.depth_of_field.enabled ? "ON" : "OFF");
                break;
            case GLFW_KEY_3:
                pp.gaussian_blur.enabled = !pp.gaussian_blur.enabled;
                printf("[PostProcess] Gaussian Blur: %s\n", pp.gaussian_blur.enabled ? "ON" : "OFF");
                break;
            case GLFW_KEY_4: {
                g_state.tonemap_index = (g_state.tonemap_index + 1) % 5;
                pp.tone_mapping.op = static_cast<ToneMapOperator>(g_state.tonemap_index);
                printf("[PostProcess] Tone Map: %s\n", tonemap_operator_name(pp.tone_mapping.op));
                break;
            }
            case GLFW_KEY_EQUAL: // +
                pp.tone_mapping.exposure += 0.1f;
                printf("[PostProcess] Exposure: %.2f\n", pp.tone_mapping.exposure);
                break;
            case GLFW_KEY_MINUS: // -
                pp.tone_mapping.exposure = std::max(0.1f, pp.tone_mapping.exposure - 0.1f);
                printf("[PostProcess] Exposure: %.2f\n", pp.tone_mapping.exposure);
                break;
            case GLFW_KEY_B:
                pp.bloom.threshold = std::max(0.1f, pp.bloom.threshold - 0.1f);
                printf("[PostProcess] Bloom threshold: %.2f\n", pp.bloom.threshold);
                break;
            case GLFW_KEY_N:
                pp.bloom.threshold += 0.1f;
                printf("[PostProcess] Bloom threshold: %.2f\n", pp.bloom.threshold);
                break;
            case GLFW_KEY_F:
                pp.depth_of_field.focus_distance = std::max(1.0f, pp.depth_of_field.focus_distance - 1.0f);
                printf("[PostProcess] DoF focus: %.1f\n", pp.depth_of_field.focus_distance);
                break;
            case GLFW_KEY_G:
                pp.depth_of_field.focus_distance += 1.0f;
                printf("[PostProcess] DoF focus: %.1f\n", pp.depth_of_field.focus_distance);
                break;
            case GLFW_KEY_R:
                g_state.pp_config = PostProcessConfig{};
                g_state.pp_config.bloom.enabled = true;
                g_state.pp_config.tone_mapping.enabled = true;
                g_state.tonemap_index = 0;
                printf("[PostProcess] Reset to defaults\n");
                break;
            case GLFW_KEY_S:
                g_state.show_hud = !g_state.show_hud;
                printf("[HUD] %s\n", g_state.show_hud ? "ON" : "OFF");
                break;
        }
    });

    // ---- 9. Main Loop ----
    auto start = std::chrono::high_resolution_clock::now();
    uint64_t frame_count = 0;

    printf("\nPost-Process Demo Scene:\n");
    printf("  - Central emissive cube (bright HDR — bloom source)\n");
    printf("  - 5 colored emissive light spheres (bloom sources)\n");
    printf("  - Foreground cube (near DoF blur)\n");
    printf("  - Background spheres (far DoF blur)\n");
    printf("  - Orbiting emissive sphere (dynamic bloom)\n");
    printf("\nControls:\n");
    printf("  1 = Toggle Bloom     2 = Toggle DoF\n");
    printf("  3 = Toggle Blur      4 = Cycle ToneMap\n");
    printf("  +/- = Exposure       B/N = Bloom threshold\n");
    printf("  F/G = DoF focus      R = Reset\n");
    printf("  Mouse: orbit camera  Scroll: zoom\n");
    printf("\nEntering main loop. Close window to exit.\n\n");

    while (!surface_provider.should_close()) {
        surface_provider.poll_events();

        auto now = std::chrono::high_resolution_clock::now();
        float elapsed = std::chrono::duration<float>(now - start).count();

        // Update post-process config each frame (in case keys changed it)
        renderer.set_postprocess_config(g_state.pp_config);

        // Orbit camera
        float cos_pitch = std::cos(g_state.pitch);
        float eye[3] = {
            g_state.center[0] + g_state.radius * cos_pitch * std::sin(g_state.yaw),
            g_state.center[1] + g_state.radius * std::sin(g_state.pitch),
            g_state.center[2] + g_state.radius * cos_pitch * std::cos(g_state.yaw)
        };
        float up[3] = {0.0f, 1.0f, 0.0f};

        float view_mat[16], proj_mat[16], view_proj[16];
        mat4_look_at(view_mat, eye, g_state.center, up);
        float aspect = float(screen_w) / float(screen_h);
        mat4_perspective(proj_mat, 0.7854f, aspect, 0.1f, 200.0f);
        mat4_multiply(view_proj, proj_mat, view_mat);

        // Dynamic orbiting emissive sphere
        float orbit_radius = 6.0f;
        float orbit_speed  = 0.6f;
        float dyn_x = orbit_radius * std::sin(elapsed * orbit_speed);
        float dyn_z = orbit_radius * std::cos(elapsed * orbit_speed);
        float dyn_y = 2.0f + 0.5f * std::sin(elapsed * 1.2f);

        {
            float4x4 dyn_transform = float4x4::identity();
            dyn_transform.set_translation(dyn_x, dyn_y, dyn_z);
            renderer.update_transform(orbit_sphere_id, dyn_transform);
        }

#ifdef PICTOR_HAS_VULKAN
        // Build SceneUBO
        SceneUBO ubo;
        memcpy(ubo.view, view_mat, sizeof(view_mat));
        memcpy(ubo.proj, proj_mat, sizeof(proj_mat));
        memcpy(ubo.viewProj, view_proj, sizeof(view_proj));
        ubo.cameraPos[0] = eye[0]; ubo.cameraPos[1] = eye[1];
        ubo.cameraPos[2] = eye[2]; ubo.cameraPos[3] = 1.0f;
        ubo.ambientColor[0] = 0.15f; ubo.ambientColor[1] = 0.17f;
        ubo.ambientColor[2] = 0.25f; ubo.ambientColor[3] = 1.0f;
        // Sun direction (toward light)
        float sun_dx = -0.4f, sun_dy = 0.8f, sun_dz = 0.5f;
        float sun_len = std::sqrt(sun_dx*sun_dx + sun_dy*sun_dy + sun_dz*sun_dz);
        ubo.sunDirection[0] = sun_dx / sun_len;
        ubo.sunDirection[1] = sun_dy / sun_len;
        ubo.sunDirection[2] = sun_dz / sun_len;
        ubo.sunDirection[3] = 0.0f;
        ubo.sunColor[0] = 1.0f; ubo.sunColor[1] = 0.95f;
        ubo.sunColor[2] = 0.85f; ubo.sunColor[3] = 3.0f; // bright HDR sun
        ubo.time = elapsed;
        ubo.dofFocusDistance = g_state.pp_config.depth_of_field.enabled
                             ? g_state.pp_config.depth_of_field.focus_distance : 0.0f;
        ubo.dofFocusRange = g_state.pp_config.depth_of_field.focus_range;
        ubo.exposure = g_state.pp_config.tone_mapping.exposure;

        // Helper to fill InstanceData
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

        // Build instance data: 11 objects total
        // [0]    = central emissive cube
        // [1]    = ground plane
        // [2-6]  = 5 emissive light spheres
        // [7]    = foreground cube (DoF near)
        // [8-10] = 3 background spheres (DoF far)
        // [11]   = dynamic orbiting sphere
        InstanceData instances[12];

        // Instance 0: Central emissive cube (bright HDR — bloom source)
        {
            float trans[16], scale[16], rot[16], tmp[16];
            mat4_translation(trans, 0.0f, 2.0f, 0.0f);
            mat4_scale(scale, 1.5f, 1.5f, 1.5f);
            mat4_rotation_y(rot, elapsed * 0.3f);
            mat4_multiply(tmp, trans, rot);
            mat4_multiply(instances[0].model, tmp, scale);
            fill_instance(instances[0], instances[0].model,
                          0.9f, 0.85f, 0.7f,
                          0.8f, 0.2f, 1.0f,
                          5.0f, 1.0f, 0.9f, 0.7f); // bright emissive
        }

        // Instance 1: Ground plane
        {
            float model[16];
            mat4_identity(model);
            fill_instance(instances[1], model,
                          0.3f, 0.3f, 0.32f,
                          0.0f, 0.8f, 1.0f,
                          0.0f, 0.0f, 0.0f, 0.0f);
        }

        // Instances 2-6: Emissive light spheres
        struct LightSphereData { float x, z, r, g, b, intensity; };
        LightSphereData ls_data[] = {
            { 5.0f,  3.0f,  1.0f, 0.2f, 0.1f, 8.0f},
            {-4.0f,  5.0f,  0.1f, 0.5f, 1.0f, 6.0f},
            { 2.0f, -4.0f,  1.0f, 0.8f, 0.0f, 10.0f},
            {-6.0f, -2.0f,  0.6f, 0.1f, 1.0f, 5.0f},
            { 7.0f, -1.0f,  0.0f, 1.0f, 0.3f, 7.0f},
        };
        for (int i = 0; i < 5; ++i) {
            float trans[16], scale[16], model[16];
            mat4_translation(trans, ls_data[i].x, 1.0f, ls_data[i].z);
            mat4_scale(scale, 1.0f, 1.0f, 1.0f);
            mat4_multiply(model, trans, scale);
            fill_instance(instances[2 + i], model,
                          ls_data[i].r, ls_data[i].g, ls_data[i].b,
                          0.1f, 0.5f, 1.0f,
                          ls_data[i].intensity,
                          ls_data[i].r, ls_data[i].g, ls_data[i].b);
        }

        // Instance 7: Foreground cube (DoF near blur)
        {
            float trans[16], scale[16], model[16];
            mat4_translation(trans, 2.0f, 0.5f, 8.0f);
            mat4_scale(scale, 1.0f, 1.0f, 1.0f);
            mat4_multiply(model, trans, scale);
            fill_instance(instances[7], model,
                          0.6f, 0.25f, 0.15f,
                          0.3f, 0.5f, 1.0f,
                          0.0f, 0.0f, 0.0f, 0.0f);
        }

        // Instances 8-10: Background spheres (DoF far blur)
        for (int i = 0; i < 3; ++i) {
            float bx = -5.0f + i * 5.0f;
            float trans[16], scale[16], model[16];
            mat4_translation(trans, bx, 1.2f, -10.0f);
            mat4_scale(scale, 2.0f, 2.0f, 2.0f);
            mat4_multiply(model, trans, scale);
            fill_instance(instances[8 + i], model,
                          0.5f, 0.5f, 0.55f,
                          0.7f, 0.3f, 1.0f,
                          0.0f, 0.0f, 0.0f, 0.0f);
        }

        // Instance 11: Dynamic orbiting emissive sphere
        {
            float trans[16], scale[16], model[16];
            mat4_translation(trans, dyn_x, dyn_y, dyn_z);
            mat4_scale(scale, 1.0f, 1.0f, 1.0f);
            mat4_multiply(model, trans, scale);
            float glow = 0.5f + 0.5f * std::sin(elapsed * 2.0f);
            fill_instance(instances[11], model,
                          0.15f, 0.15f, 0.15f,
                          0.7f, 0.3f, 1.0f,
                          6.0f * glow,
                          0.2f, 0.9f, 0.7f); // teal-ish emissive
        }

        scene_renderer.update_scene(ubo, instances, 12);

        uint32_t image_idx = vk_ctx.acquire_next_image();
        if (image_idx == UINT32_MAX) continue;

        auto cmd = vk_ctx.command_buffers()[image_idx];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &begin_info);

        // Begin render pass
        VkClearValue clear_values[2];
        clear_values[0].color = {{0.01f, 0.01f, 0.02f, 1.0f}};
        clear_values[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rp_info{};
        rp_info.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_info.renderPass  = vk_ctx.default_render_pass();
        rp_info.framebuffer = vk_ctx.framebuffers()[image_idx];
        rp_info.renderArea  = {{0, 0}, vk_ctx.swapchain_extent()};
        rp_info.clearValueCount = 2;
        rp_info.pClearValues    = clear_values;

        vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

        // Draw scene objects
        PPSceneRenderer::DrawGroup groups[] = {
            {PPSceneRenderer::DrawGroup::CUBE,   0, 1},   // instance 0: central cube
            {PPSceneRenderer::DrawGroup::FLOOR,  1, 1},   // instance 1: ground
            {PPSceneRenderer::DrawGroup::SPHERE, 2, 5},   // instances 2-6: light spheres
            {PPSceneRenderer::DrawGroup::CUBE,   7, 1},   // instance 7: foreground cube
            {PPSceneRenderer::DrawGroup::SPHERE, 8, 4},   // instances 8-11: bg spheres + orbiter
        };

        scene_renderer.draw(cmd, vk_ctx.swapchain_extent(), groups, 5);

        // Draw HUD text overlay
        if (g_state.show_hud) {
            auto extent = vk_ctx.swapchain_extent();
            text_renderer.begin(cmd, extent);

            char buf[256];
            float y = 10.0f;
            const float line_h = 36.0f;

            snprintf(buf, sizeof(buf), "FPS: %.0f", g_state.fps);
            text_renderer.draw_text(10.0f, y, buf, 1.0f, 1.0f, 1.0f);
            y += line_h;

            snprintf(buf, sizeof(buf), "[1] Bloom:    %s  (thr=%.2f int=%.1f)",
                     g_state.pp_config.bloom.enabled ? "ON " : "OFF",
                     g_state.pp_config.bloom.threshold,
                     g_state.pp_config.bloom.intensity);
            text_renderer.draw_text(10.0f, y, buf,
                                    g_state.pp_config.bloom.enabled ? 0.3f : 0.5f,
                                    g_state.pp_config.bloom.enabled ? 1.0f : 0.5f,
                                    g_state.pp_config.bloom.enabled ? 0.3f : 0.5f);
            y += line_h;

            snprintf(buf, sizeof(buf), "[2] DoF:      %s  (focus=%.1f range=%.1f)",
                     g_state.pp_config.depth_of_field.enabled ? "ON " : "OFF",
                     g_state.pp_config.depth_of_field.focus_distance,
                     g_state.pp_config.depth_of_field.focus_range);
            text_renderer.draw_text(10.0f, y, buf,
                                    g_state.pp_config.depth_of_field.enabled ? 0.3f : 0.5f,
                                    g_state.pp_config.depth_of_field.enabled ? 0.8f : 0.5f,
                                    g_state.pp_config.depth_of_field.enabled ? 1.0f : 0.5f);
            y += line_h;

            snprintf(buf, sizeof(buf), "[3] Blur:     %s  (sigma=%.1f)",
                     g_state.pp_config.gaussian_blur.enabled ? "ON " : "OFF",
                     g_state.pp_config.gaussian_blur.sigma);
            text_renderer.draw_text(10.0f, y, buf,
                                    g_state.pp_config.gaussian_blur.enabled ? 1.0f : 0.5f,
                                    g_state.pp_config.gaussian_blur.enabled ? 0.8f : 0.5f,
                                    g_state.pp_config.gaussian_blur.enabled ? 0.3f : 0.5f);
            y += line_h;

            snprintf(buf, sizeof(buf), "[4] ToneMap:  %s (%s)",
                     g_state.pp_config.tone_mapping.enabled ? "ON " : "OFF",
                     tonemap_operator_name(g_state.pp_config.tone_mapping.op));
            text_renderer.draw_text(10.0f, y, buf,
                                    g_state.pp_config.tone_mapping.enabled ? 1.0f : 0.5f,
                                    g_state.pp_config.tone_mapping.enabled ? 0.6f : 0.5f,
                                    g_state.pp_config.tone_mapping.enabled ? 1.0f : 0.5f);
            y += line_h;

            snprintf(buf, sizeof(buf), "[+/-] Exposure: %.2f",
                     g_state.pp_config.tone_mapping.exposure);
            text_renderer.draw_text(10.0f, y, buf, 0.9f, 0.9f, 0.7f);
            y += line_h;

            text_renderer.draw_text(10.0f, y,
                "[B/N] Bloom thr  [F/G] DoF focus  [R] Reset  [S] HUD",
                0.5f, 0.5f, 0.5f);

            text_renderer.end();
        }

        vkCmdEndRenderPass(cmd);

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

        // Run Pictor data pipeline (includes post-process execution)
        float dt = 1.0f / 60.0f;
        camera.position = {eye[0], eye[1], eye[2]};
        renderer.begin_frame(dt);
        renderer.render(camera);
        renderer.end_frame();

        ++frame_count;

        // Update FPS for HUD
        const auto& stats = renderer.get_frame_stats();
        g_state.fps = stats.fps;

        // Print stats periodically
        if (frame_count % 180 == 0) {
            const auto& ppc = g_state.pp_config;
            printf("[Frame %llu] FPS: %.1f  PostProcess: Bloom=%s DoF=%s Blur=%s ToneMap=%s(%s) Exp=%.2f\n",
                   static_cast<unsigned long long>(frame_count),
                   stats.fps,
                   ppc.bloom.enabled ? "ON" : "OFF",
                   ppc.depth_of_field.enabled ? "ON" : "OFF",
                   ppc.gaussian_blur.enabled ? "ON" : "OFF",
                   ppc.tone_mapping.enabled ? "ON" : "OFF",
                   tonemap_operator_name(ppc.tone_mapping.op),
                   ppc.tone_mapping.exposure);
        }
    }

    // ---- 10. Cleanup ----
    vk_ctx.device_wait_idle();
#ifdef PICTOR_HAS_VULKAN
    text_renderer.shutdown();
    scene_renderer.shutdown();
#endif
    renderer.shutdown();
    vk_ctx.shutdown();
    surface_provider.destroy();

    printf("\nPost-process demo finished. %llu frames rendered.\n",
           static_cast<unsigned long long>(frame_count));
    return 0;
}
