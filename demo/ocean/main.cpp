/// Pictor Ocean Demo — Tessellation + Gerstner Waves
///
/// Demonstrates:
///   - Hardware tessellation (TCS + TES) for adaptive LOD
///   - Distance-based tessellation: dense near camera, sparse at distance
///   - Gerstner wave displacement in tessellation evaluation shader
///   - Physically-based ocean surface shading (Fresnel, SSS, foam)
///   - Target ~100K polygons average across ocean surface
///
/// Build target: pictor_ocean_demo

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

} // anonymous namespace

// ============================================================
// Scene UBO (must match ocean shader SceneParams)
// ============================================================

struct OceanSceneUBO {
    float view[16];
    float proj[16];
    float viewProj[16];
    float cameraPos[4];     // xyz + pad
    float time;
    float tessLevelHigh;    // tessellation level for near range (e.g. 32)
    float tessLevelMedium;  // tessellation level for mid range (e.g. 16)
    float tessLevelLow;     // tessellation level for far range (e.g. 4)
    float tessDistNearMid;  // distance boundary: high -> medium
    float tessDistMidFar;   // distance boundary: medium -> low
    float pad0, pad1;
};

// ============================================================
// Ocean Grid Mesh (quad patches for tessellation)
// ============================================================

struct OceanVertex {
    float pos[3];
    float texCoord[2];
};

/// Generate a flat grid of quad patches.
/// gridSize: number of patches per side
/// worldSize: total world-space extent of the ocean
///
/// To achieve ~100K polygons average with tessellation:
///   - 32x32 grid = 1024 patches
///   - Average tessellation ~10x10 = ~100 triangles per patch
///   - 1024 * 100 = ~102,400 polygons
static void generate_ocean_grid(std::vector<OceanVertex>& vertices,
                                 std::vector<uint32_t>& indices,
                                 uint32_t gridSize, float worldSize) {
    vertices.clear();
    indices.clear();

    float half = worldSize * 0.5f;
    float step = worldSize / static_cast<float>(gridSize);

    // Generate (gridSize+1)^2 vertices
    for (uint32_t z = 0; z <= gridSize; ++z) {
        for (uint32_t x = 0; x <= gridSize; ++x) {
            OceanVertex v;
            v.pos[0] = -half + static_cast<float>(x) * step;
            v.pos[1] = 0.0f;
            v.pos[2] = -half + static_cast<float>(z) * step;
            v.texCoord[0] = static_cast<float>(x) / static_cast<float>(gridSize);
            v.texCoord[1] = static_cast<float>(z) / static_cast<float>(gridSize);
            vertices.push_back(v);
        }
    }

    // Generate quad patch indices (4 vertices per patch)
    uint32_t stride = gridSize + 1;
    for (uint32_t z = 0; z < gridSize; ++z) {
        for (uint32_t x = 0; x < gridSize; ++x) {
            uint32_t topLeft     = z * stride + x;
            uint32_t topRight    = z * stride + (x + 1);
            uint32_t bottomRight = (z + 1) * stride + (x + 1);
            uint32_t bottomLeft  = (z + 1) * stride + x;

            indices.push_back(topLeft);
            indices.push_back(topRight);
            indices.push_back(bottomRight);
            indices.push_back(bottomLeft);
        }
    }
}

// ============================================================
// Ocean Demo Renderer (Vulkan)
// ============================================================

#ifdef PICTOR_HAS_VULKAN

class OceanDemoRenderer {
public:
    bool initialize(VulkanContext& vk_ctx, const char* shader_dir) {
        vk_ctx_ = &vk_ctx;
        device_ = vk_ctx.device();

        if (!create_depth_resources()) return false;
        if (!create_render_pass()) return false;
        if (!create_framebuffers()) return false;
        if (!create_descriptor_layout()) return false;
        if (!create_pipeline(shader_dir)) return false;
        if (!create_buffers()) return false;
        if (!create_descriptor_sets()) return false;

        initialized_ = true;
        return true;
    }

    void set_wireframe(bool enabled) { wireframe_ = enabled; }
    bool wireframe() const { return wireframe_; }
    void toggle_wireframe() { wireframe_ = !wireframe_; }

    void shutdown() {
        if (!initialized_) return;
        vkDeviceWaitIdle(device_);

        if (pipeline_)           vkDestroyPipeline(device_, pipeline_, nullptr);
        if (wireframe_pipeline_) vkDestroyPipeline(device_, wireframe_pipeline_, nullptr);
        if (pipeline_layout_)    vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        if (desc_set_layout_) vkDestroyDescriptorSetLayout(device_, desc_set_layout_, nullptr);
        if (desc_pool_)       vkDestroyDescriptorPool(device_, desc_pool_, nullptr);

        auto destroy_buf = [this](VkBuffer& b, VkDeviceMemory& m) {
            if (b) vkDestroyBuffer(device_, b, nullptr);
            if (m) vkFreeMemory(device_, m, nullptr);
            b = VK_NULL_HANDLE; m = VK_NULL_HANDLE;
        };

        destroy_buf(ubo_buffer_, ubo_memory_);
        destroy_buf(grid_vb_, grid_vb_mem_);
        destroy_buf(grid_ib_, grid_ib_mem_);

        cleanup_depth_and_framebuffers();

        if (render_pass_) vkDestroyRenderPass(device_, render_pass_, nullptr);

        initialized_ = false;
    }

    ~OceanDemoRenderer() { shutdown(); }

    void update_scene(const OceanSceneUBO& ubo) {
        void* mapped = nullptr;
        vkMapMemory(device_, ubo_memory_, 0, sizeof(OceanSceneUBO), 0, &mapped);
        memcpy(mapped, &ubo, sizeof(OceanSceneUBO));
        vkUnmapMemory(device_, ubo_memory_);
    }

    void render(VkCommandBuffer cmd, uint32_t image_index, VkExtent2D extent) {
        VkClearValue clear_values[2];
        clear_values[0].color = {{0.30f, 0.50f, 0.70f, 1.0f}};  // sky blue
        clear_values[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rp_info{};
        rp_info.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_info.renderPass  = render_pass_;
        rp_info.framebuffer = framebuffers_[image_index];
        rp_info.renderArea  = {{0, 0}, extent};
        rp_info.clearValueCount = 2;
        rp_info.pClearValues    = clear_values;

        vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

        VkPipeline active_pipeline = wireframe_ ? wireframe_pipeline_ : pipeline_;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, active_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout_, 0, 1, &desc_set_, 0, nullptr);

        VkViewport viewport{0, 0, (float)extent.width, (float)extent.height, 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &grid_vb_, &offset);
        vkCmdBindIndexBuffer(cmd, grid_ib_, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, grid_index_count_, 1, 0, 0, 0);

        vkCmdEndRenderPass(cmd);
    }

    VkRenderPass render_pass() const { return render_pass_; }

private:
    // ---- Vulkan buffer helpers ----

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

    // ---- Depth buffer ----

    VkFormat find_depth_format() {
        VkFormat candidates[] = {
            VK_FORMAT_D32_SFLOAT,
            VK_FORMAT_D32_SFLOAT_S8_UINT,
            VK_FORMAT_D24_UNORM_S8_UINT
        };
        for (auto fmt : candidates) {
            VkFormatProperties props;
            vkGetPhysicalDeviceFormatProperties(vk_ctx_->physical_device(), fmt, &props);
            if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                return fmt;
            }
        }
        return VK_FORMAT_D32_SFLOAT;
    }

    bool create_depth_resources() {
        depth_format_ = find_depth_format();
        VkExtent2D extent = vk_ctx_->swapchain_extent();

        VkImageCreateInfo img_info{};
        img_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_info.imageType     = VK_IMAGE_TYPE_2D;
        img_info.format        = depth_format_;
        img_info.extent.width  = extent.width;
        img_info.extent.height = extent.height;
        img_info.extent.depth  = 1;
        img_info.mipLevels     = 1;
        img_info.arrayLayers   = 1;
        img_info.samples       = VK_SAMPLE_COUNT_1_BIT;
        img_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        img_info.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        img_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(device_, &img_info, nullptr, &depth_image_) != VK_SUCCESS)
            return false;

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, depth_image_, &req);

        VkMemoryAllocateInfo alloc{};
        alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = find_memory_type(req.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device_, &alloc, nullptr, &depth_memory_) != VK_SUCCESS)
            return false;
        vkBindImageMemory(device_, depth_image_, depth_memory_, 0);

        VkImageViewCreateInfo view_info{};
        view_info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image    = depth_image_;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format   = depth_format_;
        view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        view_info.subresourceRange.baseMipLevel   = 0;
        view_info.subresourceRange.levelCount     = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount     = 1;

        if (vkCreateImageView(device_, &view_info, nullptr, &depth_view_) != VK_SUCCESS)
            return false;

        return true;
    }

    bool create_render_pass() {
        VkAttachmentDescription attachments[2] = {};

        // Color attachment
        attachments[0].format         = vk_ctx_->swapchain_format();
        attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        // Depth attachment
        attachments[1].format         = depth_format_;
        attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference color_ref{};
        color_ref.attachment = 0;
        color_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depth_ref{};
        depth_ref.attachment = 1;
        depth_ref.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount    = 1;
        subpass.pColorAttachments       = &color_ref;
        subpass.pDepthStencilAttachment = &depth_ref;

        VkSubpassDependency dependency{};
        dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass    = 0;
        dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo info{};
        info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = 2;
        info.pAttachments    = attachments;
        info.subpassCount    = 1;
        info.pSubpasses      = &subpass;
        info.dependencyCount = 1;
        info.pDependencies   = &dependency;

        return vkCreateRenderPass(device_, &info, nullptr, &render_pass_) == VK_SUCCESS;
    }

    bool create_framebuffers() {
        auto& image_views = vk_ctx_->swapchain_image_views();
        VkExtent2D extent = vk_ctx_->swapchain_extent();
        framebuffers_.resize(image_views.size());

        for (size_t i = 0; i < image_views.size(); ++i) {
            VkImageView fb_attachments[2] = { image_views[i], depth_view_ };

            VkFramebufferCreateInfo info{};
            info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            info.renderPass      = render_pass_;
            info.attachmentCount = 2;
            info.pAttachments    = fb_attachments;
            info.width           = extent.width;
            info.height          = extent.height;
            info.layers          = 1;

            if (vkCreateFramebuffer(device_, &info, nullptr, &framebuffers_[i]) != VK_SUCCESS)
                return false;
        }
        return true;
    }

    void cleanup_depth_and_framebuffers() {
        for (auto fb : framebuffers_) {
            if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
        }
        framebuffers_.clear();

        if (depth_view_)   vkDestroyImageView(device_, depth_view_, nullptr);
        if (depth_image_)  vkDestroyImage(device_, depth_image_, nullptr);
        if (depth_memory_) vkFreeMemory(device_, depth_memory_, nullptr);
        depth_view_   = VK_NULL_HANDLE;
        depth_image_  = VK_NULL_HANDLE;
        depth_memory_ = VK_NULL_HANDLE;
    }

    // ---- Descriptor layout ----

    bool create_descriptor_layout() {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT |
                                  VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                  VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT |
                                  VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 1;
        layout_info.pBindings    = &binding;

        return vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &desc_set_layout_) == VK_SUCCESS;
    }

    // ---- Pipeline (4 shader stages: vert, tesc, tese, frag) ----

    bool create_pipeline(const char* shader_dir) {
        std::string base = std::string(shader_dir) + "/";
        VkShaderModule vert_mod      = load_shader((base + "ocean.vert.spv").c_str());
        VkShaderModule tesc_mod      = load_shader((base + "ocean.tesc.spv").c_str());
        VkShaderModule tese_mod      = load_shader((base + "ocean.tese.spv").c_str());
        VkShaderModule frag_mod      = load_shader((base + "ocean.frag.spv").c_str());
        VkShaderModule wire_frag_mod = load_shader((base + "ocean_wireframe.frag.spv").c_str());

        if (!vert_mod || !tesc_mod || !tese_mod || !frag_mod || !wire_frag_mod) {
            fprintf(stderr, "OceanDemoRenderer: failed to load shaders from %s\n", shader_dir);
            auto cleanup = [this](VkShaderModule m) {
                if (m) vkDestroyShaderModule(device_, m, nullptr);
            };
            cleanup(vert_mod); cleanup(tesc_mod); cleanup(tese_mod);
            cleanup(frag_mod); cleanup(wire_frag_mod);
            return false;
        }

        // Pipeline layout
        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts    = &desc_set_layout_;

        if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
            vkDestroyShaderModule(device_, vert_mod, nullptr);
            vkDestroyShaderModule(device_, tesc_mod, nullptr);
            vkDestroyShaderModule(device_, tese_mod, nullptr);
            vkDestroyShaderModule(device_, frag_mod, nullptr);
            vkDestroyShaderModule(device_, wire_frag_mod, nullptr);
            return false;
        }

        // Shared pipeline state
        VkVertexInputBindingDescription bind_desc{};
        bind_desc.binding   = 0;
        bind_desc.stride    = sizeof(OceanVertex);
        bind_desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attr_descs[2] = {};
        attr_descs[0].location = 0;
        attr_descs[0].binding  = 0;
        attr_descs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
        attr_descs[0].offset   = offsetof(OceanVertex, pos);
        attr_descs[1].location = 1;
        attr_descs[1].binding  = 0;
        attr_descs[1].format   = VK_FORMAT_R32G32_SFLOAT;
        attr_descs[1].offset   = offsetof(OceanVertex, texCoord);

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount   = 1;
        vi.pVertexBindingDescriptions      = &bind_desc;
        vi.vertexAttributeDescriptionCount = 2;
        vi.pVertexAttributeDescriptions    = attr_descs;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;

        VkPipelineTessellationStateCreateInfo tess{};
        tess.sType              = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
        tess.patchControlPoints = 4;

        VkPipelineViewportStateCreateInfo vp{};
        vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount  = 1;

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

        // --- Fill pipeline (original) ---
        VkPipelineShaderStageCreateInfo fill_stages[4] = {};
        fill_stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fill_stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        fill_stages[0].module = vert_mod;
        fill_stages[0].pName  = "main";
        fill_stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fill_stages[1].stage  = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        fill_stages[1].module = tesc_mod;
        fill_stages[1].pName  = "main";
        fill_stages[2].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fill_stages[2].stage  = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        fill_stages[2].module = tese_mod;
        fill_stages[2].pName  = "main";
        fill_stages[3].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fill_stages[3].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        fill_stages[3].module = frag_mod;
        fill_stages[3].pName  = "main";

        VkPipelineRasterizationStateCreateInfo rs_fill{};
        rs_fill.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs_fill.polygonMode = VK_POLYGON_MODE_FILL;
        rs_fill.cullMode    = VK_CULL_MODE_NONE;
        rs_fill.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs_fill.lineWidth   = 1.0f;

        VkGraphicsPipelineCreateInfo pipe_info{};
        pipe_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipe_info.stageCount          = 4;
        pipe_info.pStages             = fill_stages;
        pipe_info.pVertexInputState   = &vi;
        pipe_info.pInputAssemblyState = &ia;
        pipe_info.pTessellationState  = &tess;
        pipe_info.pViewportState      = &vp;
        pipe_info.pRasterizationState = &rs_fill;
        pipe_info.pMultisampleState   = &ms;
        pipe_info.pDepthStencilState  = &ds;
        pipe_info.pColorBlendState    = &cb;
        pipe_info.pDynamicState       = &dyn;
        pipe_info.layout              = pipeline_layout_;
        pipe_info.renderPass          = render_pass_;

        VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1,
                                                     &pipe_info, nullptr, &pipeline_);
        if (result != VK_SUCCESS) {
            vkDestroyShaderModule(device_, vert_mod, nullptr);
            vkDestroyShaderModule(device_, tesc_mod, nullptr);
            vkDestroyShaderModule(device_, tese_mod, nullptr);
            vkDestroyShaderModule(device_, frag_mod, nullptr);
            vkDestroyShaderModule(device_, wire_frag_mod, nullptr);
            return false;
        }

        // --- Wireframe pipeline ---
        VkPipelineShaderStageCreateInfo wire_stages[4] = {};
        wire_stages[0] = fill_stages[0]; // same vert
        wire_stages[1] = fill_stages[1]; // same tesc
        wire_stages[2] = fill_stages[2]; // same tese
        wire_stages[3].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        wire_stages[3].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        wire_stages[3].module = wire_frag_mod;
        wire_stages[3].pName  = "main";

        VkPipelineRasterizationStateCreateInfo rs_wire{};
        rs_wire.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs_wire.polygonMode = VK_POLYGON_MODE_LINE;
        rs_wire.cullMode    = VK_CULL_MODE_NONE;
        rs_wire.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs_wire.lineWidth   = 1.0f;

        pipe_info.pStages             = wire_stages;
        pipe_info.pRasterizationState = &rs_wire;

        result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1,
                                            &pipe_info, nullptr, &wireframe_pipeline_);

        vkDestroyShaderModule(device_, vert_mod, nullptr);
        vkDestroyShaderModule(device_, tesc_mod, nullptr);
        vkDestroyShaderModule(device_, tese_mod, nullptr);
        vkDestroyShaderModule(device_, frag_mod, nullptr);
        vkDestroyShaderModule(device_, wire_frag_mod, nullptr);

        return result == VK_SUCCESS;
    }

    // ---- Buffers ----

    bool create_buffers() {
        VkMemoryPropertyFlags host_vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        // UBO
        if (!create_buffer(sizeof(OceanSceneUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           host_vis, ubo_buffer_, ubo_memory_))
            return false;

        // Ocean grid mesh
        // 32x32 grid with average tess ~10 => ~100K polygons
        std::vector<OceanVertex> grid_verts;
        std::vector<uint32_t> grid_idxs;
        generate_ocean_grid(grid_verts, grid_idxs, GRID_SIZE, OCEAN_WORLD_SIZE);
        grid_index_count_ = static_cast<uint32_t>(grid_idxs.size());

        printf("  Ocean grid: %ux%u patches (%u control vertices, %u indices)\n",
               GRID_SIZE, GRID_SIZE,
               static_cast<uint32_t>(grid_verts.size()),
               grid_index_count_);

        if (!create_buffer(grid_verts.size() * sizeof(OceanVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           host_vis, grid_vb_, grid_vb_mem_))
            return false;
        upload_buffer(grid_vb_, grid_vb_mem_, grid_verts.data(), grid_verts.size() * sizeof(OceanVertex));

        if (!create_buffer(grid_idxs.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                           host_vis, grid_ib_, grid_ib_mem_))
            return false;
        upload_buffer(grid_ib_, grid_ib_mem_, grid_idxs.data(), grid_idxs.size() * sizeof(uint32_t));

        return true;
    }

    // ---- Descriptor sets ----

    bool create_descriptor_sets() {
        VkDescriptorPoolSize pool_size{};
        pool_size.type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        pool_size.descriptorCount = 1;

        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets       = 1;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes    = &pool_size;

        if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &desc_pool_) != VK_SUCCESS)
            return false;

        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool     = desc_pool_;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts        = &desc_set_layout_;

        if (vkAllocateDescriptorSets(device_, &alloc_info, &desc_set_) != VK_SUCCESS)
            return false;

        VkDescriptorBufferInfo ubo_info{ubo_buffer_, 0, sizeof(OceanSceneUBO)};

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = desc_set_;
        write.dstBinding      = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo     = &ubo_info;

        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        return true;
    }

    // ---- Members ----

    VulkanContext* vk_ctx_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    bool initialized_ = false;

    // Depth buffer
    VkImage        depth_image_  = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory_ = VK_NULL_HANDLE;
    VkImageView    depth_view_   = VK_NULL_HANDLE;
    VkFormat       depth_format_ = VK_FORMAT_D32_SFLOAT;

    // Render pass & framebuffers (with depth)
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;

    // Pipeline
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline wireframe_pipeline_ = VK_NULL_HANDLE;
    bool wireframe_ = false;
    VkDescriptorSetLayout desc_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet desc_set_ = VK_NULL_HANDLE;

    // Buffers
    VkBuffer ubo_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory ubo_memory_ = VK_NULL_HANDLE;
    VkBuffer grid_vb_ = VK_NULL_HANDLE, grid_ib_ = VK_NULL_HANDLE;
    VkDeviceMemory grid_vb_mem_ = VK_NULL_HANDLE, grid_ib_mem_ = VK_NULL_HANDLE;
    uint32_t grid_index_count_ = 0;

    // Grid configuration:
    // 32x32 = 1024 patches, average tess ~10x10 = ~100 tris/patch => ~100K polygons
    static constexpr uint32_t GRID_SIZE = 32;
    static constexpr float OCEAN_WORLD_SIZE = 400.0f;
};

#endif // PICTOR_HAS_VULKAN

// ============================================================
// Main
// ============================================================

int main() {
    printf("=== Pictor Ocean Demo — Tessellation + Gerstner Waves ===\n\n");

    // ---- 1. GLFW Window ----
    GlfwSurfaceProvider surface_provider;
    GlfwWindowConfig win_cfg;
    win_cfg.width  = 1920;
    win_cfg.height = 1080;
    win_cfg.title  = "Pictor — Ocean Wave Demo (Tessellation)";
    win_cfg.vsync  = true;

    if (!surface_provider.create(win_cfg)) {
        fprintf(stderr, "Failed to create GLFW window\n");
        return 1;
    }

    // ---- 2. Vulkan Context ----
    VulkanContext vk_ctx;
    VulkanContextConfig vk_cfg;
    vk_cfg.app_name   = "Pictor Ocean Demo";
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

    // ---- 3. Ocean Demo Renderer ----
#ifdef PICTOR_HAS_VULKAN
    OceanDemoRenderer ocean_renderer;
    std::string shader_dir = "shaders";
    if (!ocean_renderer.initialize(vk_ctx, shader_dir.c_str())) {
        shader_dir = "../shaders";
        if (!ocean_renderer.initialize(vk_ctx, shader_dir.c_str())) {
            fprintf(stderr, "Failed to initialize Ocean renderer\n");
            vk_ctx.shutdown();
            surface_provider.destroy();
            return 1;
        }
    }
    printf("Ocean Demo Renderer initialized.\n");
#endif

    // ---- 4. Pictor Renderer (data pipeline) ----
    RendererConfig pictor_cfg;
    pictor_cfg.initial_profile  = "Standard";
    pictor_cfg.screen_width     = screen_w;
    pictor_cfg.screen_height    = screen_h;
    pictor_cfg.profiler_enabled = true;
    pictor_cfg.overlay_mode     = OverlayMode::STANDARD;

    PictorRenderer renderer;
    renderer.initialize(pictor_cfg);

    // Camera
    Camera camera;
    camera.position = {0.0f, 8.0f, 20.0f};
    for (int i = 0; i < 6; ++i) {
        camera.frustum.planes[i].normal   = {0, 0, 0};
        camera.frustum.planes[i].distance = 10000.0f;
    }
    camera.frustum.planes[0] = {{1, 0, 0}, 50.0f};
    camera.frustum.planes[1] = {{-1, 0, 0}, 50.0f};
    camera.frustum.planes[2] = {{0, 1, 0}, 50.0f};
    camera.frustum.planes[3] = {{0, -1, 0}, 50.0f};
    camera.frustum.planes[4] = {{0, 0, 1}, 0.1f};
    camera.frustum.planes[5] = {{0, 0, -1}, 500.0f};

    // ---- 5. Main Loop ----
    auto start = std::chrono::high_resolution_clock::now();
    uint64_t frame_count = 0;

    // ---- Orbit Camera State ----
    struct OrbitCamera {
        float yaw   = 0.0f;
        float pitch = 0.3f;    // slight downward angle
        float radius = 40.0f;
        float center[3] = {0.0f, 0.0f, 0.0f};
        double lastMouseX = 0.0, lastMouseY = 0.0;
        bool dragging = false;
    };
    static OrbitCamera orbit_cam;

    // Mouse callbacks for orbit camera
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
        // Clamp pitch to avoid flipping
        if (orbit_cam.pitch > 1.5f)  orbit_cam.pitch = 1.5f;
        if (orbit_cam.pitch < -1.5f) orbit_cam.pitch = -1.5f;
    });
    glfwSetScrollCallback(win, [](GLFWwindow*, double /*xoffset*/, double yoffset) {
        orbit_cam.radius -= static_cast<float>(yoffset) * 3.0f;
        if (orbit_cam.radius < 5.0f)  orbit_cam.radius = 5.0f;
        if (orbit_cam.radius > 200.0f) orbit_cam.radius = 200.0f;
    });

    printf("\nOcean setup:\n");
    printf("  - 32x32 quad patch grid (1024 patches)\n");
    printf("  - 3-level tessellation: High(32) / Medium(16) / Low(4)\n");
    printf("  - Target: ~100K polygons adaptive LOD\n");
    printf("  - 6-layer Gerstner wave displacement\n");
    printf("  - Physically-based ocean shading (Fresnel + SSS + foam)\n");
    printf("  - Press 'W' to toggle wireframe tessellation view\n");
    printf("  - Mouse drag: orbit camera, Scroll: zoom\n");
    printf("\nEntering main loop. Close the window to exit.\n\n");

    bool w_key_was_pressed = false;

    while (!surface_provider.should_close()) {
        surface_provider.poll_events();

        // Toggle wireframe with W key
        {
            bool w_pressed = glfwGetKey(win, GLFW_KEY_W) == GLFW_PRESS;
            if (w_pressed && !w_key_was_pressed) {
                ocean_renderer.toggle_wireframe();
                printf("[Wireframe] %s\n", ocean_renderer.wireframe() ? "ON" : "OFF");
            }
            w_key_was_pressed = w_pressed;
        }

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
        mat4_perspective(proj_mat, 0.7854f, aspect, 0.1f, 500.0f);
        mat4_multiply(view_proj, proj_mat, view_mat);

        // Build UBO — 3-level tessellation
        OceanSceneUBO ubo;
        memcpy(ubo.view,    view_mat,  sizeof(view_mat));
        memcpy(ubo.proj,    proj_mat,  sizeof(proj_mat));
        memcpy(ubo.viewProj, view_proj, sizeof(view_proj));
        ubo.cameraPos[0] = eye[0];
        ubo.cameraPos[1] = eye[1];
        ubo.cameraPos[2] = eye[2];
        ubo.cameraPos[3] = 0.0f;
        ubo.time            = elapsed;
        ubo.tessLevelHigh   = 32.0f;
        ubo.tessLevelMedium = 16.0f;
        ubo.tessLevelLow    = 4.0f;
        ubo.tessDistNearMid = 30.0f;
        ubo.tessDistMidFar  = 100.0f;
        ubo.pad0 = ubo.pad1 = 0.0f;

        // Update Pictor renderer
        camera.position = {eye[0], eye[1], eye[2]};

        // Calculate delta time
        static auto last_time = start;
        float dt = std::chrono::duration<float>(now - last_time).count();
        last_time = now;

        renderer.begin_frame(dt);
        renderer.render(camera);
        renderer.end_frame();

        // Render ocean
#ifdef PICTOR_HAS_VULKAN
        uint32_t img_idx = vk_ctx.acquire_next_image();
        if (img_idx == UINT32_MAX) continue;

        ocean_renderer.update_scene(ubo);

        VkCommandBuffer cmd = vk_ctx.command_buffers()[img_idx];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &begin_info);

        ocean_renderer.render(cmd, img_idx, vk_ctx.swapchain_extent());

        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphore wait_sem = vk_ctx.image_available_semaphore();
        VkSemaphore signal_sem = vk_ctx.render_finished_semaphore();

        VkSubmitInfo submit{};
        submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount   = 1;
        submit.pWaitSemaphores      = &wait_sem;
        submit.pWaitDstStageMask    = &wait_stage;
        submit.commandBufferCount   = 1;
        submit.pCommandBuffers      = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores    = &signal_sem;

        vkQueueSubmit(vk_ctx.graphics_queue(), 1, &submit, vk_ctx.in_flight_fence());
        vk_ctx.present(img_idx);
#endif

        frame_count++;
        if (frame_count % 300 == 0) {
            printf("[Frame %lu] t=%.1fs\n", (unsigned long)frame_count, elapsed);
        }
    }

    // ---- 6. Cleanup ----
    vk_ctx.device_wait_idle();

#ifdef PICTOR_HAS_VULKAN
    ocean_renderer.shutdown();
#endif

    renderer.shutdown();
    vk_ctx.shutdown();
    surface_provider.destroy();

    printf("\nOcean demo finished. Total frames: %lu\n", (unsigned long)frame_count);
    return 0;
}
