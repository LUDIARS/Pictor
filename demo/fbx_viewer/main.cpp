// Pictor FBX Viewer Demo
//
// End-to-end pipeline:
//   1. Load FBX via FBXImporter (Level 4 facade).
//   2. Register skeleton + clips with AnimationSystem.
//   3. Pack triangulated geometry into the canonical SkinnedVertex layout.
//   4. Upload to Vulkan; create a skinning-capable pipeline using the
//      general-purpose lit.vert / lit.frag shaders from pictor/shaders/.
//   5. Each frame, push updated bone matrices to an SSBO and render.
//
// Camera orbits the model so we can verify both the skeleton hierarchy
// and the animation playback are wired correctly.
//
// Usage:
//   pictor_fbx_viewer <model.fbx>  [shader_dir]
// If no file is given, a synthetic 3-bone bent rod is shown.

#include "pictor/animation/fbx_importer.h"
#include "pictor/animation/fbx_scene.h"
#include "pictor/animation/animation_system.h"
#include "pictor/animation/skinned_vertex.h"
#include "pictor/core/types.h"
#include "pictor/surface/glfw_surface_provider.h"
#include "pictor/surface/vulkan_context.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef PICTOR_HAS_VULKAN
int main() {
    std::fprintf(stderr, "This demo requires Vulkan (PICTOR_HAS_VULKAN).\n");
    return 1;
}
#else

using namespace pictor;

// ============================================================
// Math helpers (column-major 4x4, stored as float[16])
// ============================================================

namespace {

void mat4_identity(float* m) {
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void mat4_multiply(float* out, const float* a, const float* b) {
    float tmp[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            tmp[j * 4 + i] = 0.0f;
            for (int k = 0; k < 4; ++k) tmp[j * 4 + i] += a[k * 4 + i] * b[j * 4 + k];
        }
    std::memcpy(out, tmp, 16 * sizeof(float));
}

void mat4_look_at(float* out, const float* eye, const float* center, const float* up) {
    float fx = center[0] - eye[0], fy = center[1] - eye[1], fz = center[2] - eye[2];
    float fl = std::sqrt(fx*fx + fy*fy + fz*fz);
    if (fl > 0) { fx /= fl; fy /= fl; fz /= fl; }
    float sx = fy*up[2] - fz*up[1], sy = fz*up[0] - fx*up[2], sz = fx*up[1] - fy*up[0];
    float sl = std::sqrt(sx*sx + sy*sy + sz*sz);
    if (sl > 0) { sx /= sl; sy /= sl; sz /= sl; }
    float ux = sy*fz - sz*fy, uy = sz*fx - sx*fz, uz = sx*fy - sy*fx;
    mat4_identity(out);
    out[0] = sx;  out[4] = sy;  out[8]  = sz;  out[12] = -(sx*eye[0]+sy*eye[1]+sz*eye[2]);
    out[1] = ux;  out[5] = uy;  out[9]  = uz;  out[13] = -(ux*eye[0]+uy*eye[1]+uz*eye[2]);
    out[2] = -fx; out[6] = -fy; out[10] = -fz; out[14] =  (fx*eye[0]+fy*eye[1]+fz*eye[2]);
}

void mat4_perspective(float* out, float fovy_rad, float aspect, float near_z, float far_z) {
    std::memset(out, 0, 16 * sizeof(float));
    float f = 1.0f / std::tan(fovy_rad * 0.5f);
    out[0]  = f / aspect;
    out[5]  = -f;                                 // Vulkan Y-flip
    out[10] = far_z / (near_z - far_z);
    out[11] = -1.0f;
    out[14] = (near_z * far_z) / (near_z - far_z);
}

// Convert Pictor float4x4 (row-major with translation in m[3][*]) to
// column-major flat array expected by GLSL mat4.
void pictor_mat_to_glsl(float* out, const pictor::float4x4& m) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[c * 4 + r] = m.m[r][c];
}

// ============================================================
// Mesh packing (FBX triangulated cache -> SkinnedVertex array)
// ============================================================

struct PackedMesh {
    std::vector<SkinnedVertex> vertices;
    std::vector<uint32_t>      indices;
    float3                     center{};
    float                      radius = 1.0f;
};

PackedMesh pack_mesh_from_fbx(const FBXImportResult& r) {
    PackedMesh out;
    if (!r.scene) return out;

    const SkinMeshDescriptor* best = nullptr;
    for (const auto& sm : r.skin_meshes) {
        if (!best || sm.vertex_count > best->vertex_count) best = &sm;
    }
    if (!best || best->vertex_count == 0) return out;

    // Triangulated cache lives inside FBXScene, owned by geometry objects.
    // We need to find the FBXGeometry that produced this SkinMeshDescriptor.
    // Simpler: iterate all geometries and use the one matching vertex_count.
    const FBXGeometry::Triangulated* tri = nullptr;
    for (FBXObjectId gid : r.scene->ids_of_type(FBXObjectType::GEOMETRY)) {
        const FBXGeometry::Triangulated* t = r.scene->triangulate(gid);
        if (t && t->valid && t->positions.size() == best->vertex_count) {
            tri = t;
            break;
        }
    }
    if (!tri) return out;

    // Normals might be empty; synthesize flat ones from adjacent triangle.
    std::vector<float3> normals = tri->normals;
    if (normals.size() != tri->positions.size()) {
        normals.assign(tri->positions.size(), {0, 1, 0});
        for (size_t i = 0; i + 2 < tri->indices.size(); i += 3) {
            const int32_t ia = tri->indices[i + 0];
            const int32_t ib = tri->indices[i + 1];
            const int32_t ic = tri->indices[i + 2];
            const float3& A = tri->positions[ia];
            const float3& B = tri->positions[ib];
            const float3& C = tri->positions[ic];
            float3 e1{B.x - A.x, B.y - A.y, B.z - A.z};
            float3 e2{C.x - A.x, C.y - A.y, C.z - A.z};
            float3 n{e1.y*e2.z - e1.z*e2.y, e1.z*e2.x - e1.x*e2.z, e1.x*e2.y - e1.y*e2.x};
            float l = std::sqrt(n.x*n.x + n.y*n.y + n.z*n.z);
            if (l > 0) { n.x/=l; n.y/=l; n.z/=l; }
            normals[ia] = n; normals[ib] = n; normals[ic] = n;
        }
    }

    out.vertices = pack_skinned_vertices(
        tri->positions.data(), tri->positions.size(),
        normals.data(),        normals.size(),
        best->skin_weights.data(), best->skin_weights.size());

    out.indices.assign(tri->indices.begin(), tri->indices.end());

    // Bounding sphere (for camera fit).
    float3 mn = tri->positions.empty() ? float3{} : tri->positions[0];
    float3 mx = mn;
    for (const auto& p : tri->positions) {
        mn = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
        mx = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
    }
    out.center = {(mn.x+mx.x)*0.5f, (mn.y+mx.y)*0.5f, (mn.z+mx.z)*0.5f};
    float dx = mx.x-mn.x, dy = mx.y-mn.y, dz = mx.z-mn.z;
    out.radius = 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);
    if (out.radius < 0.01f) out.radius = 1.0f;
    return out;
}

// Synthesize a simple bent rod mesh for the no-FBX-file fallback.
PackedMesh build_synthetic_mesh(uint32_t bone_count) {
    PackedMesh out;
    const uint32_t segments_per_bone = 6;
    const uint32_t radial = 8;
    const float seg_len = 1.0f;
    const float r_outer = 0.15f;
    const uint32_t segments_total = segments_per_bone * bone_count;
    for (uint32_t s = 0; s <= segments_total; ++s) {
        float t = static_cast<float>(s) / segments_total;
        float x = t * seg_len * bone_count;
        float bone_f = t * bone_count;
        uint32_t bone0 = std::min<uint32_t>(static_cast<uint32_t>(bone_f), bone_count - 1);
        uint32_t bone1 = std::min<uint32_t>(bone0 + 1, bone_count - 1);
        float w1 = bone_f - static_cast<float>(bone0);
        float w0 = 1.0f - w1;
        for (uint32_t a = 0; a < radial; ++a) {
            float ang = 2.0f * 3.1415926f * (static_cast<float>(a) / radial);
            SkinnedVertex v{};
            v.position[0] = x;
            v.position[1] = std::cos(ang) * r_outer;
            v.position[2] = std::sin(ang) * r_outer;
            v.normal[0] = 0; v.normal[1] = std::cos(ang); v.normal[2] = std::sin(ang);
            v.joint_indices[0] = bone0;
            v.joint_indices[1] = bone1;
            v.joint_indices[2] = 0; v.joint_indices[3] = 0;
            v.joint_weights[0] = w0;
            v.joint_weights[1] = w1;
            v.joint_weights[2] = 0; v.joint_weights[3] = 0;
            out.vertices.push_back(v);
        }
    }
    for (uint32_t s = 0; s < segments_total; ++s) {
        for (uint32_t a = 0; a < radial; ++a) {
            uint32_t a0 =  s      * radial + a;
            uint32_t a1 =  s      * radial + (a + 1) % radial;
            uint32_t b0 = (s + 1) * radial + a;
            uint32_t b1 = (s + 1) * radial + (a + 1) % radial;
            out.indices.push_back(a0);
            out.indices.push_back(b0);
            out.indices.push_back(a1);
            out.indices.push_back(a1);
            out.indices.push_back(b0);
            out.indices.push_back(b1);
        }
    }
    out.center = {seg_len * bone_count * 0.5f, 0, 0};
    out.radius = seg_len * bone_count * 0.75f;
    return out;
}

} // namespace

// ============================================================
// Shader / buffer layouts (CPU side; must match lit.vert)
// ============================================================

struct SceneUBO {
    float view[16];
    float proj[16];
    float light_dir[4];   // xyz + intensity
    float light_color[4]; // rgb + ambient_intensity
    float camera_pos[4];
};

struct InstanceData {
    float    model[16];
    float    base_color[4];
    uint32_t skin_info[4]; // {bone_offset, bone_count, 0, 0}
};

constexpr uint32_t kMaxBones = 256;

// ============================================================
// Vulkan helpers
// ============================================================

static uint32_t find_memory_type(VkPhysicalDevice pd, uint32_t type_filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(pd, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    return UINT32_MAX;
}

static bool create_buffer(VkDevice device, VkPhysicalDevice pd,
                          VkDeviceSize size, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags props,
                          VkBuffer& buf, VkDeviceMemory& mem) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size  = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &info, nullptr, &buf) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buf, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = find_memory_type(pd, req.memoryTypeBits, props);
    if (alloc.memoryTypeIndex == UINT32_MAX) return false;
    if (vkAllocateMemory(device, &alloc, nullptr, &mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(device, buf, mem, 0);
    return true;
}

static VkShaderModule load_shader_spv(VkDevice device, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "Failed to open shader: %s\n", path.c_str()); return VK_NULL_HANDLE; }
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<char> code(static_cast<size_t>(len));
    std::fread(code.data(), 1, code.size(), f);
    std::fclose(f);
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &mod) != VK_SUCCESS) return VK_NULL_HANDLE;
    return mod;
}

// ============================================================
// FBXViewer
// ============================================================

class FBXViewer {
public:
    bool initialize(FBXImportResult fbx, PackedMesh mesh, const std::string& shader_dir) {
        fbx_  = std::move(fbx);
        mesh_ = std::move(mesh);
        shader_dir_ = shader_dir;

        GlfwWindowConfig wc; wc.width = 1280; wc.height = 720; wc.title = "Pictor FBX Viewer";
        if (!provider_.create(wc)) { std::fprintf(stderr, "GLFW window create failed\n"); return false; }
        VulkanContextConfig vcfg; vcfg.app_name = "pictor_fbx_viewer";
        if (!vk_.initialize(&provider_, vcfg)) { std::fprintf(stderr, "Vulkan init failed\n"); return false; }

        if (!create_depth_resources())   return false;
        if (!create_render_pass())       return false;
        if (!create_framebuffers())      return false;
        if (!create_descriptor_layout()) return false;
        if (!create_pipeline())          return false;
        if (!create_buffers())           return false;
        if (!create_descriptor_sets())   return false;

        // Animation setup.
        AnimationSystemConfig acfg;
        acfg.gpu_skinning_enabled   = false;
        acfg.max_bones_per_skeleton = kMaxBones;
        acfg.max_active_instances   = 8;
        anim_.initialize(acfg);

        if (!fbx_.skeleton.bones.empty()) {
            skel_ = anim_.register_skeleton(fbx_.skeleton);
            for (const auto& c : fbx_.clips) clip_handles_.push_back(anim_.register_clip(c));
            inst_ = anim_.create_instance(42, skel_);
            if (!clip_handles_.empty()) anim_.play(inst_, clip_handles_[0], 1.0f, 1.0f);
        }

        return true;
    }

    void run() {
        auto t0 = std::chrono::steady_clock::now();
        auto prev = t0;
        while (!provider_.should_close()) {
            provider_.poll_events();
            auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - prev).count();
            float elapsed = std::chrono::duration<float>(now - t0).count();
            prev = now;

            anim_.update(dt);

            uint32_t img_idx = vk_.acquire_next_image();
            if (img_idx == UINT32_MAX) {
                handle_resize();
                continue;
            }
            update_uniforms(elapsed);
            record_and_submit(img_idx);
            vk_.present(img_idx);
        }
        vkDeviceWaitIdle(vk_.device());
    }

    void shutdown() {
        VkDevice d = vk_.device();
        if (d) vkDeviceWaitIdle(d);
        if (inst_ != INVALID_ANIMATION_STATE) anim_.destroy_instance(inst_);
        anim_.shutdown();

        auto safe_buf = [&](VkBuffer& b, VkDeviceMemory& m) {
            if (b) { vkDestroyBuffer(d, b, nullptr); b = VK_NULL_HANDLE; }
            if (m) { vkFreeMemory(d, m, nullptr);    m = VK_NULL_HANDLE; }
        };
        safe_buf(ubo_buffer_, ubo_mem_);
        safe_buf(instance_buffer_, instance_mem_);
        safe_buf(bone_buffer_, bone_mem_);
        safe_buf(vb_, vb_mem_);
        safe_buf(ib_, ib_mem_);

        if (pipeline_)        vkDestroyPipeline(d, pipeline_, nullptr);
        if (pipeline_layout_) vkDestroyPipelineLayout(d, pipeline_layout_, nullptr);
        if (desc_layout_)     vkDestroyDescriptorSetLayout(d, desc_layout_, nullptr);
        if (desc_pool_)       vkDestroyDescriptorPool(d, desc_pool_, nullptr);

        for (auto fb : framebuffers_) if (fb) vkDestroyFramebuffer(d, fb, nullptr);
        framebuffers_.clear();
        if (render_pass_) vkDestroyRenderPass(d, render_pass_, nullptr);
        destroy_depth_resources();

        vk_.shutdown();
    }

private:
    // ─── depth buffer (own, so we can add a depth attachment) ─
    bool create_depth_resources() {
        VkDevice d = vk_.device();
        VkExtent2D ext = vk_.swapchain_extent();
        const VkFormat depth_fmt = VK_FORMAT_D32_SFLOAT;

        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format    = depth_fmt;
        ii.extent    = {ext.width, ext.height, 1};
        ii.mipLevels = 1; ii.arrayLayers = 1;
        ii.samples   = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling    = VK_IMAGE_TILING_OPTIMAL;
        ii.usage     = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(d, &ii, nullptr, &depth_image_) != VK_SUCCESS) return false;

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(d, depth_image_, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = find_memory_type(vk_.physical_device(), req.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(d, &ai, nullptr, &depth_mem_) != VK_SUCCESS) return false;
        vkBindImageMemory(d, depth_image_, depth_mem_, 0);

        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = depth_image_;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format   = depth_fmt;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(d, &vi, nullptr, &depth_view_) != VK_SUCCESS) return false;
        return true;
    }

    void destroy_depth_resources() {
        VkDevice d = vk_.device();
        if (depth_view_)  { vkDestroyImageView(d, depth_view_,  nullptr); depth_view_  = VK_NULL_HANDLE; }
        if (depth_image_) { vkDestroyImage(d, depth_image_,     nullptr); depth_image_ = VK_NULL_HANDLE; }
        if (depth_mem_)   { vkFreeMemory(d, depth_mem_,         nullptr); depth_mem_   = VK_NULL_HANDLE; }
    }

    // ─── custom render pass (color + depth) ──────────────────
    bool create_render_pass() {
        VkDevice d = vk_.device();
        VkAttachmentDescription attachments[2]{};
        attachments[0].format  = vk_.swapchain_format();
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp  = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        attachments[1].format  = VK_FORMAT_D32_SFLOAT;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp  = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        VkSubpassDescription sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &color_ref;
        sub.pDepthStencilAttachment = &depth_ref;

        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.srcAccessMask = 0;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rp{};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp.attachmentCount = 2;
        rp.pAttachments = attachments;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        rp.dependencyCount = 1;
        rp.pDependencies = &dep;
        return vkCreateRenderPass(d, &rp, nullptr, &render_pass_) == VK_SUCCESS;
    }

    bool create_framebuffers() {
        VkDevice d = vk_.device();
        VkExtent2D ext = vk_.swapchain_extent();
        const auto& views = vk_.swapchain_image_views();
        framebuffers_.resize(views.size());
        for (size_t i = 0; i < views.size(); ++i) {
            VkImageView attachments[2] = {views[i], depth_view_};
            VkFramebufferCreateInfo fb{};
            fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fb.renderPass = render_pass_;
            fb.attachmentCount = 2;
            fb.pAttachments = attachments;
            fb.width  = ext.width;
            fb.height = ext.height;
            fb.layers = 1;
            if (vkCreateFramebuffer(d, &fb, nullptr, &framebuffers_[i]) != VK_SUCCESS) return false;
        }
        return true;
    }

    // ─── descriptor layout / pool / sets ─────────────────────
    bool create_descriptor_layout() {
        VkDevice d = vk_.device();
        VkDescriptorSetLayoutBinding b[3]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; b[0].descriptorCount = 1;
        b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[1].descriptorCount = 1;
        b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[2].descriptorCount = 1;
        b[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 3;
        info.pBindings = b;
        return vkCreateDescriptorSetLayout(d, &info, nullptr, &desc_layout_) == VK_SUCCESS;
    }

    bool create_descriptor_sets() {
        VkDevice d = vk_.device();
        VkDescriptorPoolSize sz[2]{};
        sz[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sz[0].descriptorCount = 1;
        sz[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sz[1].descriptorCount = 2;
        VkDescriptorPoolCreateInfo dp{};
        dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dp.poolSizeCount = 2; dp.pPoolSizes = sz; dp.maxSets = 1;
        if (vkCreateDescriptorPool(d, &dp, nullptr, &desc_pool_) != VK_SUCCESS) return false;

        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = desc_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &desc_layout_;
        if (vkAllocateDescriptorSets(d, &ai, &desc_set_) != VK_SUCCESS) return false;

        VkDescriptorBufferInfo ubo_info{ubo_buffer_, 0, sizeof(SceneUBO)};
        VkDescriptorBufferInfo inst_info{instance_buffer_, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo bone_info{bone_buffer_, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[3]{};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet = desc_set_; w[0].dstBinding = 0;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].descriptorCount = 1; w[0].pBufferInfo = &ubo_info;
        w[1] = w[0]; w[1].dstBinding = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[1].pBufferInfo = &inst_info;
        w[2] = w[0]; w[2].dstBinding = 2; w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[2].pBufferInfo = &bone_info;
        vkUpdateDescriptorSets(d, 3, w, 0, nullptr);
        return true;
    }

    // ─── pipeline ────────────────────────────────────────────
    bool create_pipeline() {
        VkDevice d = vk_.device();

        VkShaderModule vs = load_shader_spv(d, shader_dir_ + "/lit.vert.spv");
        VkShaderModule fs = load_shader_spv(d, shader_dir_ + "/lit.frag.spv");
        if (!vs || !fs) return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride  = SKINNED_VERTEX_STRIDE;
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attrs[4]{};
        attrs[0].location = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;    attrs[0].offset = SKINNED_VERTEX_OFFSET_POSITION;
        attrs[1].location = 1; attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;    attrs[1].offset = SKINNED_VERTEX_OFFSET_NORMAL;
        attrs[2].location = 2; attrs[2].format = VK_FORMAT_R32G32B32A32_UINT;   attrs[2].offset = SKINNED_VERTEX_OFFSET_JOINTS;
        attrs[3].location = 3; attrs[3].format = VK_FORMAT_R32G32B32A32_SFLOAT; attrs[3].offset = SKINNED_VERTEX_OFFSET_WEIGHTS;

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = 4; vi.pVertexAttributeDescriptions = attrs;

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
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp  = VK_COMPARE_OP_LESS;

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
        pl.setLayoutCount = 1; pl.pSetLayouts = &desc_layout_;
        if (vkCreatePipelineLayout(d, &pl, nullptr, &pipeline_layout_) != VK_SUCCESS) return false;

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = 2; info.pStages = stages;
        info.pVertexInputState = &vi;
        info.pInputAssemblyState = &ia;
        info.pViewportState = &vp;
        info.pRasterizationState = &rs;
        info.pMultisampleState = &ms;
        info.pDepthStencilState = &ds;
        info.pColorBlendState = &cb;
        info.pDynamicState = &dyn;
        info.layout = pipeline_layout_;
        info.renderPass = render_pass_;
        info.subpass = 0;

        VkResult r = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
        vkDestroyShaderModule(d, vs, nullptr);
        vkDestroyShaderModule(d, fs, nullptr);
        return r == VK_SUCCESS;
    }

    // ─── buffers (vertex / index / UBO / SSBO) ───────────────
    bool create_buffers() {
        VkDevice d = vk_.device();
        VkPhysicalDevice pd = vk_.physical_device();
        const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        if (mesh_.vertices.empty() || mesh_.indices.empty()) {
            std::fprintf(stderr, "No mesh data to upload.\n");
            return false;
        }

        // Vertex buffer
        VkDeviceSize vb_size = mesh_.vertices.size() * sizeof(SkinnedVertex);
        if (!create_buffer(d, pd, vb_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, hv, vb_, vb_mem_)) return false;
        void* p = nullptr;
        vkMapMemory(d, vb_mem_, 0, vb_size, 0, &p);
        std::memcpy(p, mesh_.vertices.data(), vb_size);
        vkUnmapMemory(d, vb_mem_);

        // Index buffer
        VkDeviceSize ib_size = mesh_.indices.size() * sizeof(uint32_t);
        if (!create_buffer(d, pd, ib_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, hv, ib_, ib_mem_)) return false;
        vkMapMemory(d, ib_mem_, 0, ib_size, 0, &p);
        std::memcpy(p, mesh_.indices.data(), ib_size);
        vkUnmapMemory(d, ib_mem_);
        index_count_ = static_cast<uint32_t>(mesh_.indices.size());

        // UBO
        if (!create_buffer(d, pd, sizeof(SceneUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, hv,
                           ubo_buffer_, ubo_mem_)) return false;
        // Instance SSBO (1 instance for this demo)
        if (!create_buffer(d, pd, sizeof(InstanceData) * 1, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hv,
                           instance_buffer_, instance_mem_)) return false;
        // Bone matrix SSBO
        if (!create_buffer(d, pd, sizeof(float) * 16 * kMaxBones, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hv,
                           bone_buffer_, bone_mem_)) return false;
        return true;
    }

    // ─── per-frame uniform upload ────────────────────────────
    void update_uniforms(float t) {
        VkDevice d = vk_.device();

        // Camera orbits the model.
        VkExtent2D ext = vk_.swapchain_extent();
        float aspect = (ext.height == 0) ? 1.0f : static_cast<float>(ext.width) / ext.height;
        float dist = mesh_.radius * 2.5f + 0.1f;
        float eye[3] = {
            mesh_.center.x + dist * std::cos(t * 0.4f),
            mesh_.center.y + mesh_.radius * 0.5f,
            mesh_.center.z + dist * std::sin(t * 0.4f),
        };
        float center[3] = {mesh_.center.x, mesh_.center.y, mesh_.center.z};
        float up[3] = {0, 1, 0};

        SceneUBO ubo{};
        mat4_look_at(ubo.view, eye, center, up);
        mat4_perspective(ubo.proj, 45.0f * 3.14159265f / 180.0f, aspect, 0.1f, dist * 8.0f + 1000.0f);
        ubo.light_dir[0] = 0.4f; ubo.light_dir[1] = 0.8f; ubo.light_dir[2] = 0.5f; ubo.light_dir[3] = 1.0f;
        float l = std::sqrt(ubo.light_dir[0]*ubo.light_dir[0] + ubo.light_dir[1]*ubo.light_dir[1] + ubo.light_dir[2]*ubo.light_dir[2]);
        ubo.light_dir[0] /= l; ubo.light_dir[1] /= l; ubo.light_dir[2] /= l;
        ubo.light_color[0] = 1.0f; ubo.light_color[1] = 0.97f; ubo.light_color[2] = 0.93f; ubo.light_color[3] = 0.25f;
        ubo.camera_pos[0] = eye[0]; ubo.camera_pos[1] = eye[1]; ubo.camera_pos[2] = eye[2]; ubo.camera_pos[3] = 1.0f;
        void* p = nullptr;
        vkMapMemory(d, ubo_mem_, 0, sizeof(SceneUBO), 0, &p);
        std::memcpy(p, &ubo, sizeof(SceneUBO));
        vkUnmapMemory(d, ubo_mem_);

        // Instance data (identity model, tinted by instance).
        InstanceData inst{};
        mat4_identity(inst.model);
        inst.base_color[0] = 0.85f; inst.base_color[1] = 0.75f; inst.base_color[2] = 0.6f; inst.base_color[3] = 1.0f;
        inst.skin_info[0] = 0;
        inst.skin_info[1] = static_cast<uint32_t>(anim_.get_bone_count(inst_));
        vkMapMemory(d, instance_mem_, 0, sizeof(InstanceData), 0, &p);
        std::memcpy(p, &inst, sizeof(InstanceData));
        vkUnmapMemory(d, instance_mem_);

        // Bone matrices.
        const uint32_t bone_count = std::min<uint32_t>(anim_.get_bone_count(inst_), kMaxBones);
        std::vector<float> bones(16 * kMaxBones, 0.0f);
        for (uint32_t b = 0; b < kMaxBones; ++b) bones[b * 16 + 0] = bones[b * 16 + 5] = bones[b * 16 + 10] = bones[b * 16 + 15] = 1.0f;
        const float4x4* sk = anim_.get_skinning_matrices(inst_);
        if (sk) {
            for (uint32_t b = 0; b < bone_count; ++b) {
                pictor_mat_to_glsl(&bones[b * 16], sk[b]);
            }
        }
        vkMapMemory(d, bone_mem_, 0, sizeof(float) * 16 * kMaxBones, 0, &p);
        std::memcpy(p, bones.data(), sizeof(float) * 16 * kMaxBones);
        vkUnmapMemory(d, bone_mem_);
    }

    // ─── command buffer record + submit ──────────────────────
    void record_and_submit(uint32_t img_idx) {
        VkCommandBuffer cmd = vk_.command_buffers()[img_idx];
        VkExtent2D ext = vk_.swapchain_extent();

        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &bi);

        VkClearValue clears[2];
        clears[0].color = {{0.08f, 0.09f, 0.12f, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rp{};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass = render_pass_;
        rp.framebuffer = framebuffers_[img_idx];
        rp.renderArea = {{0, 0}, ext};
        rp.clearValueCount = 2;
        rp.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{0, 0, static_cast<float>(ext.width), static_cast<float>(ext.height), 0, 1};
        VkRect2D sc{{0, 0}, ext};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                                0, 1, &desc_set_, 0, nullptr);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb_, &off);
        vkCmdBindIndexBuffer(cmd, ib_, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, index_count_, 1, 0, 0, 0);

        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);

        VkSemaphore wait_sem = vk_.image_available_semaphore();
        VkSemaphore sig_sem  = vk_.render_finished_semaphore();
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &wait_sem; si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &sig_sem;
        vkQueueSubmit(vk_.graphics_queue(), 1, &si, vk_.in_flight_fence());
    }

    void handle_resize() {
        VkDevice d = vk_.device();
        vkDeviceWaitIdle(d);
        for (auto fb : framebuffers_) if (fb) vkDestroyFramebuffer(d, fb, nullptr);
        framebuffers_.clear();
        destroy_depth_resources();

        vk_.recreate_swapchain();
        create_depth_resources();
        create_framebuffers();
    }

private:
    GlfwSurfaceProvider provider_;
    VulkanContext       vk_;

    FBXImportResult    fbx_;
    PackedMesh         mesh_;
    std::string        shader_dir_;

    AnimationSystem                   anim_;
    SkeletonHandle                    skel_ = INVALID_SKELETON;
    std::vector<AnimationClipHandle>  clip_handles_;
    AnimationStateHandle              inst_ = INVALID_ANIMATION_STATE;

    VkRenderPass                render_pass_    = VK_NULL_HANDLE;
    std::vector<VkFramebuffer>  framebuffers_;

    VkImage         depth_image_ = VK_NULL_HANDLE;
    VkDeviceMemory  depth_mem_   = VK_NULL_HANDLE;
    VkImageView     depth_view_  = VK_NULL_HANDLE;

    VkDescriptorSetLayout desc_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool      desc_pool_   = VK_NULL_HANDLE;
    VkDescriptorSet       desc_set_    = VK_NULL_HANDLE;
    VkPipelineLayout      pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline            pipeline_        = VK_NULL_HANDLE;

    VkBuffer        ubo_buffer_ = VK_NULL_HANDLE; VkDeviceMemory ubo_mem_      = VK_NULL_HANDLE;
    VkBuffer        vb_         = VK_NULL_HANDLE; VkDeviceMemory vb_mem_       = VK_NULL_HANDLE;
    VkBuffer        ib_         = VK_NULL_HANDLE; VkDeviceMemory ib_mem_       = VK_NULL_HANDLE;
    VkBuffer        instance_buffer_ = VK_NULL_HANDLE; VkDeviceMemory instance_mem_ = VK_NULL_HANDLE;
    VkBuffer        bone_buffer_     = VK_NULL_HANDLE; VkDeviceMemory bone_mem_     = VK_NULL_HANDLE;

    uint32_t        index_count_ = 0;
};

// ============================================================
// Synthetic fallback for when no FBX file is provided.
// ============================================================

static FBXImportResult build_synthetic_fbx() {
    FBXImportResult r;
    r.success = true;
    r.detected_format = AnimationFormat::UNKNOWN;

    r.skeleton.name = "synthetic";
    r.skeleton.bones.resize(4);
    for (int i = 0; i < 4; ++i) {
        Bone& b = r.skeleton.bones[i];
        b.name = "bone_" + std::to_string(i);
        b.parent_index = i - 1;
        b.bind_pose = Transform::identity();
        b.bind_pose.translation = {(i == 0 ? 0.0f : 1.0f), 0.0f, 0.0f};
        // Bone local space is defined so that position (i,0,0) in world space
        // maps to origin in bone space.
        b.inverse_bind_matrix = float4x4::identity();
        b.inverse_bind_matrix.m[3][0] = -static_cast<float>(i);
    }

    AnimationClipDescriptor clip;
    clip.name = "wave";
    clip.duration = 2.0f;
    clip.wrap_mode = WrapMode::LOOP;
    clip.sample_rate = 30.0f;
    for (int bone_idx = 1; bone_idx <= 3; ++bone_idx) {
        AnimationChannel ch;
        ch.target_index = static_cast<uint32_t>(bone_idx);
        ch.target = ChannelTarget::ROTATION;
        ch.interpolation = InterpolationMode::LINEAR;
        for (int k = 0; k <= 12; ++k) {
            float t = static_cast<float>(k) / 6.0f;
            float angle = std::sin((t + bone_idx * 0.3f) * 3.1415926f) * 0.6f;
            Quaternion q = Quaternion::from_axis_angle({0, 0, 1}, angle);
            Keyframe kf;
            kf.time = t;
            kf.value[0] = q.x; kf.value[1] = q.y; kf.value[2] = q.z; kf.value[3] = q.w;
            ch.keyframes.push_back(kf);
        }
        clip.channels.push_back(std::move(ch));
    }
    r.clips.push_back(std::move(clip));
    return r;
}

// ============================================================
// main
// ============================================================

int main(int argc, char** argv) {
    std::printf("Pictor FBX Viewer\n");

    FBXImportResult result;
    if (argc >= 2) {
        std::printf("Loading FBX: %s\n", argv[1]);
        FBXImporter importer;
        result = importer.import_file(argv[1]);
        if (!result.success) {
            std::fprintf(stderr, "FBX import failed: %s\n", result.error_message.c_str());
            std::printf("Falling back to synthetic scene.\n");
            result = build_synthetic_fbx();
        }
    } else {
        std::printf("No FBX file specified. Usage: pictor_fbx_viewer <file.fbx> [shader_dir]\n");
        std::printf("Running with synthetic scene.\n");
        result = build_synthetic_fbx();
    }

    PackedMesh mesh;
    if (!result.skin_meshes.empty() && result.scene) {
        mesh = pack_mesh_from_fbx(result);
    }
    if (mesh.vertices.empty()) {
        std::printf("No renderable mesh in FBX — using synthetic geometry.\n");
        uint32_t bone_count = std::max<uint32_t>(1u, static_cast<uint32_t>(result.skeleton.bones.size()));
        mesh = build_synthetic_mesh(bone_count);
    }

    std::printf("Mesh: %zu verts, %zu indices, bones=%zu, clips=%zu\n",
                mesh.vertices.size(), mesh.indices.size(),
                result.skeleton.bones.size(), result.clips.size());

    std::string shader_dir = "shaders";
    if (argc >= 3) shader_dir = argv[2];

    FBXViewer viewer;
    if (!viewer.initialize(std::move(result), std::move(mesh), shader_dir)) {
        std::fprintf(stderr, "Viewer init failed.\n");
        return 1;
    }
    viewer.run();
    viewer.shutdown();
    return 0;
}

#endif // PICTOR_HAS_VULKAN
