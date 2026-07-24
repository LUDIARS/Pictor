#include "pictor/gi/sharc_executor.h"

#ifdef PICTOR_HAS_VULKAN
#include "pictor/surface/vulkan_context.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace pictor {

SharcGpuExecutor::~SharcGpuExecutor() {
    shutdown();
}

#ifdef PICTOR_HAS_VULKAN

namespace {

// 0 = UBO, 1..17 = SSBO, 18 = combined image sampler (アルベド配列)
constexpr uint32_t kBindingCount = 19;
constexpr uint32_t kSsboBindingCount = 18;

VkShaderModule load_shader_module(VkDevice device, const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return VK_NULL_HANDLE;
    const std::streamsize size = file.tellg();
    file.seekg(0);
    std::vector<char> code(static_cast<size_t>(size));
    file.read(code.data(), size);
    if (!file.good() || size == 0) return VK_NULL_HANDLE;

    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = static_cast<size_t>(size);
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &mod) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return mod;
}

uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t filter,
                          VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((filter & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return 0;
}

} // namespace

bool SharcGpuExecutor::create_buffer_(Buffer& out, VkDeviceSize size,
                                      VkBufferUsageFlags usage,
                                      bool device_local) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size        = size;
    // device-local はクリア (vkCmdFillBuffer) / staging 転送を受けるため
    // TRANSFER_DST を常に付ける
    bi.usage       = usage | (device_local
                                  ? VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                  : VkBufferUsageFlags{0});
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bi, nullptr, &out.buf) != VK_SUCCESS)
        return false;

    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(device_, out.buf, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = mr.size;
    // ホットバッファは device-local (PCIe 越しアクセスがフレーム時間の
    // 支配項だった)。 CPU が触るものだけ host-visible + coherent。
    ai.memoryTypeIndex = find_memory_type(
        vk_->physical_device(), mr.memoryTypeBits,
        device_local ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
                     : (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    if (vkAllocateMemory(device_, &ai, nullptr, &out.mem) != VK_SUCCESS)
        return false;
    vkBindBufferMemory(device_, out.buf, out.mem, 0);
    out.size = size;
    if (!device_local) {
        if (vkMapMemory(device_, out.mem, 0, size, 0, &out.mapped) !=
            VK_SUCCESS) {
            return false;
        }
        std::memset(out.mapped, 0, static_cast<size_t>(size));
    }
    // device-local のゼロ初期化は initialize 末尾の一括 FillBuffer で行う
    return true;
}

void SharcGpuExecutor::destroy_buffer_(Buffer& b) {
    if (b.mapped) vkUnmapMemory(device_, b.mem);
    if (b.buf)    vkDestroyBuffer(device_, b.buf, nullptr);
    if (b.mem)    vkFreeMemory(device_, b.mem, nullptr);
    b = Buffer{};
}

bool SharcGpuExecutor::create_device_buffer_(Buffer& out, const void* data,
                                             VkDeviceSize size) {
    // device-local 本体 (シーンは数百 MB 級 — PCIe 越し読みを避ける)
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size        = size;
    bi.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bi, nullptr, &out.buf) != VK_SUCCESS)
        return false;
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(device_, out.buf, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = find_memory_type(
        vk_->physical_device(), mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &ai, nullptr, &out.mem) != VK_SUCCESS)
        return false;
    vkBindBufferMemory(device_, out.buf, out.mem, 0);
    out.size = size;

    // staging → copy (1 回きりの転送なので同期実行で良い)
    Buffer staging;
    if (!create_buffer_(staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        return false;
    std::memcpy(staging.mapped, data, static_cast<size_t>(size));

    VkCommandBufferAllocateInfo cai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool        = vk_->command_pool();
    cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device_, &cai, &cmd) != VK_SUCCESS) {
        destroy_buffer_(staging);
        return false;
    }
    VkCommandBufferBeginInfo bgi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bgi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bgi);
    VkBufferCopy region{0, 0, size};
    vkCmdCopyBuffer(cmd, staging.buf, out.buf, 1, &region);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    vkQueueSubmit(vk_->graphics_queue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk_->graphics_queue());
    vkFreeCommandBuffers(device_, vk_->command_pool(), 1, &cmd);
    destroy_buffer_(staging);
    return true;
}

bool SharcGpuExecutor::create_pass_(Pass& out, const std::string& spv_path) {
    VkShaderModule cs = load_shader_module(device_, spv_path);
    if (cs == VK_NULL_HANDLE) {
        // 必須前提の欠落 — 黙って縮退しない (RULE_CODE §7.1 fail-fast)。
        std::fprintf(stderr, "[sharc] compute shader not found: %s\n",
                     spv_path.c_str());
        return false;
    }
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_COMPUTE_BIT, cs, "main", nullptr};
    ci.layout = layout_;
    const VkResult pr = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1,
                                                 &ci, nullptr, &out.pipeline);
    vkDestroyShaderModule(device_, cs, nullptr);
    if (pr != VK_SUCCESS) {
        std::fprintf(stderr, "[sharc] compute pipeline creation failed: %s\n",
                     spv_path.c_str());
        return false;
    }
    return true;
}

void SharcGpuExecutor::write_params_() {
    SharcParamsGpu p{};
    p.camera_pos[0]  = camera_pos_.x;
    p.camera_pos[1]  = camera_pos_.y;
    p.camera_pos[2]  = camera_pos_.z;
    p.base_cell_size = config_.base_cell_size;
    p.level_count    = config_.level_count;
    p.table_size     = config_.table_size;
    p.frame_index    = frame_index_;
    p.ema_alpha      = config_.ema_alpha;
    p.level_bias     = config_.level_bias;
    p.sss_mfp_scale  = config_.sss_mfp_scale;
    p.max_ray_steps  = config_.max_ray_steps;
    p.light_count    = light_count_;
    p.ray_count      = ray_count_;
    p.stale_frames   = config_.stale_frames;
    p.hit_epsilon    = config_.hit_epsilon;
    p.scene_tri_count = scene_tri_count_;
    p.scene_flags     = scene_flags_;
    p.floor_y         = floor_y_;
    p.scene_ray_far   = scene_ray_far_;
    std::memcpy(p.cam_fwd, cam_fwd_, sizeof(cam_fwd_));
    std::memcpy(p.cam_right, cam_right_, sizeof(cam_right_));
    std::memcpy(p.cam_up, cam_up_, sizeof(cam_up_));
    std::memcpy(params_.mapped, &p, sizeof(p));
}

void SharcGpuExecutor::set_camera(const float3& fwd, const float3& right,
                                  const float3& up, float fov_scale,
                                  float aspect, uint32_t render_width) {
    cam_fwd_[0] = fwd.x;   cam_fwd_[1] = fwd.y;   cam_fwd_[2] = fwd.z;
    cam_fwd_[3] = fov_scale;
    cam_right_[0] = right.x; cam_right_[1] = right.y; cam_right_[2] = right.z;
    cam_right_[3] = aspect;
    cam_up_[0] = up.x; cam_up_[1] = up.y; cam_up_[2] = up.z;
    cam_up_[3] = static_cast<float>(render_width);
    // 反映は次の begin_frame / set_counts の write_params_ で行われる
}

void SharcGpuExecutor::write_scene_descriptors_() {
    const Buffer* scene[4] = {&scene_nodes_, &scene_tris_, &scene_tri_mats_,
                              &scene_materials_};
    VkDescriptorBufferInfo infos[4]{};
    VkWriteDescriptorSet writes[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        infos[i] = {scene[i]->buf, 0, scene[i]->size};
        writes[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet          = desc_set_;
        writes[i].dstBinding      = 14 + i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo     = &infos[i];
    }
    vkUpdateDescriptorSets(device_, 4, writes, 0, nullptr);
}

bool SharcGpuExecutor::upload_scene(const SharcSceneUpload& scene) {
    // 入力検証 (review P1-06)
    if (!initialized_ || scene.node_count == 0 || scene.tri_count == 0 ||
        scene.nodes == nullptr || scene.tris == nullptr ||
        scene.tri_materials == nullptr || scene.materials == nullptr ||
        scene.material_count == 0) {
        std::fprintf(stderr, "[sharc] upload_scene: invalid input\n");
        return false;
    }
    // 新バッファ群を一時所有で全て完成させてから旧を破棄して切り替える —
    // 途中失敗で旧シーンと descriptor が壊れないように (review P1-06)
    Buffer new_nodes, new_tris, new_tri_mats, new_materials;
    const bool ok =
        create_device_buffer_(new_nodes, scene.nodes,
                              VkDeviceSize{scene.node_count} *
                                  sizeof(SharcBvhNodeGpu)) &&
        create_device_buffer_(new_tris, scene.tris,
                              VkDeviceSize{scene.tri_count} *
                                  sizeof(SharcTriGpu)) &&
        create_device_buffer_(new_tri_mats, scene.tri_materials,
                              VkDeviceSize{scene.tri_count} *
                                  sizeof(uint32_t)) &&
        create_device_buffer_(new_materials, scene.materials,
                              VkDeviceSize{scene.material_count} *
                                  sizeof(SharcMaterialGpu));
    if (!ok) {
        std::fprintf(stderr, "[sharc] scene upload failed (kept old scene)\n");
        destroy_buffer_(new_nodes);
        destroy_buffer_(new_tris);
        destroy_buffer_(new_tri_mats);
        destroy_buffer_(new_materials);
        return false;
    }
    vkDeviceWaitIdle(device_);
    destroy_buffer_(scene_nodes_);
    destroy_buffer_(scene_tris_);
    destroy_buffer_(scene_tri_mats_);
    destroy_buffer_(scene_materials_);
    scene_nodes_     = new_nodes;
    scene_tris_      = new_tris;
    scene_tri_mats_  = new_tri_mats;
    scene_materials_ = new_materials;
    write_scene_descriptors_();
    if (scene.atlas_pixels != nullptr && scene.atlas_layers > 0) {
        if (!create_atlas_(scene.atlas_pixels, scene.atlas_size,
                           scene.atlas_layers)) {
            std::fprintf(stderr, "[sharc] atlas upload failed\n");
            return false;
        }
        std::fprintf(stderr, "[sharc] atlas: %u layers @ %ux%u\n",
                     scene.atlas_layers, scene.atlas_size, scene.atlas_size);
    }
    scene_tri_count_ = scene.tri_count;
    scene_flags_ |= kSharcSceneMesh;
    write_params_();
    std::fprintf(stderr,
                 "[sharc] scene uploaded: %u nodes, %u tris (%.1f MB "
                 "device-local)\n",
                 scene.node_count, scene.tri_count,
                 static_cast<double>(scene_nodes_.size + scene_tris_.size +
                                     scene_tri_mats_.size +
                                     scene_materials_.size) /
                     (1024.0 * 1024.0));
    return true;
}

void SharcGpuExecutor::set_scene_floor(bool enabled, float floor_y) {
    if (enabled) scene_flags_ |= kSharcSceneFloor;
    else         scene_flags_ &= ~kSharcSceneFloor;
    floor_y_ = floor_y;
    if (initialized_) write_params_();
}

void SharcGpuExecutor::set_scene_far(float ray_far) {
    scene_ray_far_ = ray_far;
    if (initialized_) write_params_();
}

void SharcGpuExecutor::set_albedo_view(bool enabled) {
    if (enabled) scene_flags_ |= kSharcSceneAlbedo;
    else         scene_flags_ &= ~kSharcSceneAlbedo;
    if (initialized_) write_params_();
}

void SharcGpuExecutor::destroy_atlas_() {
    if (atlas_view_)  vkDestroyImageView(device_, atlas_view_, nullptr);
    if (atlas_image_) vkDestroyImage(device_, atlas_image_, nullptr);
    if (atlas_mem_)   vkFreeMemory(device_, atlas_mem_, nullptr);
    atlas_view_  = VK_NULL_HANDLE;
    atlas_image_ = VK_NULL_HANDLE;
    atlas_mem_   = VK_NULL_HANDLE;
}

bool SharcGpuExecutor::create_atlas_(const uint8_t* pixels, uint32_t size,
                                     uint32_t layers) {
    destroy_atlas_();
    uint32_t mips = 1;
    while ((size >> mips) >= 1u && mips < 10u) ++mips;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = VK_FORMAT_R8G8B8A8_SRGB;
    ici.extent        = {size, size, 1};
    ici.mipLevels     = mips;
    ici.arrayLayers   = layers;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &atlas_image_) != VK_SUCCESS)
        return false;

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(device_, atlas_image_, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = find_memory_type(vk_->physical_device(),
                                          mr.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &ai, nullptr, &atlas_mem_) != VK_SUCCESS)
        return false;
    vkBindImageMemory(device_, atlas_image_, atlas_mem_, 0);

    // ── staging → mip0 転送 + blit でミップ生成 ──
    const VkDeviceSize bytes =
        VkDeviceSize{size} * size * 4 * layers;
    Buffer staging;
    if (!create_buffer_(staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        return false;
    std::memcpy(staging.mapped, pixels, static_cast<size_t>(bytes));

    VkCommandBufferAllocateInfo cai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool        = vk_->command_pool();
    cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device_, &cai, &cmd) != VK_SUCCESS) {
        destroy_buffer_(staging);
        return false;
    }
    VkCommandBufferBeginInfo bgi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bgi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bgi);

    auto image_barrier = [&](uint32_t base_mip, uint32_t mip_count,
                             VkImageLayout from, VkImageLayout to,
                             VkAccessFlags src, VkAccessFlags dst) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask       = src;
        b.dstAccessMask       = dst;
        b.oldLayout           = from;
        b.newLayout           = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = atlas_image_;
        b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, base_mip,
                                 mip_count, 0, layers};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
    };

    image_barrier(0, mips, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                  VK_ACCESS_TRANSFER_WRITE_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
    region.imageExtent      = {size, size, 1};
    vkCmdCopyBufferToImage(cmd, staging.buf, atlas_image_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // mip チェーン: m-1 (SRC) → m (DST) の blit を層まとめて
    for (uint32_t m = 1; m < mips; ++m) {
        image_barrier(m - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, layers};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, layers};
        const auto dim = [size](uint32_t mip) {
            return static_cast<int32_t>(std::max(size >> mip, 1u));
        };
        blit.srcOffsets[1] = {dim(m - 1), dim(m - 1), 1};
        blit.dstOffsets[1] = {dim(m), dim(m), 1};
        vkCmdBlitImage(cmd, atlas_image_,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, atlas_image_,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
    }
    // 0..mips-2 = SRC, 最終 mip = DST → 全て SHADER_READ_ONLY へ
    if (mips > 1) {
        image_barrier(0, mips - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    image_barrier(mips - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    vkQueueSubmit(vk_->graphics_queue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk_->graphics_queue());
    vkFreeCommandBuffers(device_, vk_->command_pool(), 1, &cmd);
    destroy_buffer_(staging);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image            = atlas_image_;
    vci.viewType         = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vci.format           = VK_FORMAT_R8G8B8A8_SRGB;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers};
    if (vkCreateImageView(device_, &vci, nullptr, &atlas_view_) !=
        VK_SUCCESS) {
        return false;
    }

    VkDescriptorImageInfo dii{atlas_sampler_, atlas_view_,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet          = desc_set_;
    write.dstBinding      = 18;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &dii;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return true;
}

bool SharcGpuExecutor::initialize(VulkanContext& vk,
                                  const std::string& shader_dir,
                                  const SharcConfig& config) {
    vk_     = &vk;
    device_ = vk.device();
    config_ = config;

    if ((config_.table_size & (config_.table_size - 1)) != 0) {
        std::fprintf(stderr, "[sharc] table_size must be a power of two\n");
        return false;
    }

    // ── buffers ──
    //    ホットパス (ハッシュ / セル / レイ / 出力) は device-local。
    //    CPU が触るのは params / lights / staging / counters 読み出しのみ。
    const VkDeviceSize slots = config_.table_size;
    const auto ssbo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const VkDeviceSize rays_bytes =
        VkDeviceSize{config_.max_rays} * sizeof(SharcRayGpu);
    const VkDeviceSize shade_bytes =
        VkDeviceSize{config_.max_rays} * sizeof(SharcShadeRequestGpu);
    if (!create_buffer_(params_, sizeof(SharcParamsGpu),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
        !create_buffer_(keys_, slots * sizeof(uint32_t), ssbo, true) ||
        !create_buffer_(cells_, slots * kSharcCellBytes, ssbo, true) ||
        !create_buffer_(rays_, rays_bytes, ssbo, true) ||
        !create_buffer_(hits_, VkDeviceSize{config_.max_hits} *
                        kSharcHitRecordBytes, ssbo, true) ||
        !create_buffer_(counters_, 4 * sizeof(uint32_t),
                        ssbo | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true) ||
        !create_buffer_(requests_, slots * sizeof(uint32_t), ssbo, true) ||
        !create_buffer_(stamps_, slots * sizeof(uint32_t), ssbo, true) ||
        !create_buffer_(indirect_, 3 * sizeof(uint32_t),
                        ssbo | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true) ||
        !create_buffer_(lights_, VkDeviceSize{config_.max_lights} *
                        sizeof(SharcLightGpu), ssbo) ||
        !create_buffer_(reservoirs_, slots * kSharcReservoirUints *
                        sizeof(uint32_t), ssbo, true) ||
        !create_buffer_(cell_pos_, slots * 2 * sizeof(uint32_t), ssbo, true) ||
        !create_buffer_(shade_, shade_bytes, ssbo, true) ||
        !create_buffer_(output_, VkDeviceSize{config_.max_rays} *
                        4 * sizeof(float), ssbo, true) ||
        // シーンバッファは upload_scene まではプレースホルダ (descriptor の
        // 有効性のため最小サイズで確保しておく)
        !create_buffer_(scene_nodes_, 32, ssbo) ||
        !create_buffer_(scene_tris_, 64, ssbo) ||
        !create_buffer_(scene_tri_mats_, 16, ssbo) ||
        !create_buffer_(scene_materials_, 32, ssbo) ||
        // D1 (CPU 一次交差) の転送元 staging + counters 読み出し
        !create_buffer_(rays_staging_, rays_bytes,
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        !create_buffer_(shade_staging_, shade_bytes,
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        !create_buffer_(counters_rb_, 4 * sizeof(uint32_t),
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        std::fprintf(stderr, "[sharc] buffer creation failed\n");
        shutdown();
        return false;
    }
    write_params_();

    // device-local バッファのゼロ初期化 (ハッシュキー等は 0 = 空が前提)
    {
        VkCommandBufferAllocateInfo cai{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool        = vk.command_pool();
        cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device_, &cai, &cmd) != VK_SUCCESS) {
            shutdown();
            return false;
        }
        VkCommandBufferBeginInfo bgi{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bgi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bgi);
        for (Buffer* b : {&keys_, &cells_, &rays_, &hits_, &counters_,
                          &requests_, &stamps_, &indirect_, &reservoirs_,
                          &cell_pos_, &shade_, &output_}) {
            vkCmdFillBuffer(cmd, b->buf, 0, VK_WHOLE_SIZE, 0u);
        }
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &cmd;
        vkQueueSubmit(vk.graphics_queue(), 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(vk.graphics_queue());
        vkFreeCommandBuffers(device_, vk.command_pool(), 1, &cmd);
    }

    // ── descriptor set layout (UBO + SSBO×17 + sampler2DArray) ──
    VkDescriptorSetLayoutBinding bindings[kBindingCount]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    for (uint32_t i = 1; i < kSsboBindingCount; ++i) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    bindings[18] = {18, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dli{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = kBindingCount;
    dli.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(device_, &dli, nullptr, &dsl_) !=
        VK_SUCCESS) {
        shutdown();
        return false;
    }

    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts    = &dsl_;
    if (vkCreatePipelineLayout(device_, &pli, nullptr, &layout_) != VK_SUCCESS) {
        shutdown();
        return false;
    }

    // ── compute pipelines (hit + 4 パス) ──
    if (!create_pass_(hit_,     shader_dir + "/sharc_hit.comp.spv") ||
        !create_pass_(march_,   shader_dir + "/sharc_march.comp.spv") ||
        !create_pass_(compact_, shader_dir + "/sharc_compact.comp.spv") ||
        !create_pass_(update_,  shader_dir + "/sharc_update.comp.spv") ||
        !create_pass_(resolve_, shader_dir + "/sharc_resolve.comp.spv")) {
        shutdown();
        return false;
    }

    // ── descriptor pool + set ──
    VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSsboBindingCount - 1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
    };
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets       = 1;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes    = sizes;
    if (vkCreateDescriptorPool(device_, &dpi, nullptr, &desc_pool_) !=
        VK_SUCCESS) {
        shutdown();
        return false;
    }
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool     = desc_pool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts        = &dsl_;
    if (vkAllocateDescriptorSets(device_, &dai, &desc_set_) != VK_SUCCESS) {
        shutdown();
        return false;
    }

    const Buffer* ordered[kSsboBindingCount] = {
        &params_, &keys_, &cells_, &rays_, &hits_, &counters_, &requests_,
        &stamps_, &indirect_, &lights_, &reservoirs_, &cell_pos_, &shade_,
        &output_, &scene_nodes_, &scene_tris_, &scene_tri_mats_,
        &scene_materials_,
    };
    VkDescriptorBufferInfo infos[kSsboBindingCount]{};
    VkWriteDescriptorSet writes[kSsboBindingCount]{};
    for (uint32_t i = 0; i < kSsboBindingCount; ++i) {
        infos[i] = {ordered[i]->buf, 0, ordered[i]->size};
        writes[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet          = desc_set_;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = (i == 0) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                             : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo     = &infos[i];
    }
    vkUpdateDescriptorSets(device_, kSsboBindingCount, writes, 0, nullptr);

    // ── binding 18: アルベド配列 (初期は 1x1 白ダミー) + サンプラ ──
    {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter    = VK_FILTER_LINEAR;
        sci.minFilter    = VK_FILTER_LINEAR;
        sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.maxLod       = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(device_, &sci, nullptr, &atlas_sampler_) !=
            VK_SUCCESS) {
            shutdown();
            return false;
        }
        const uint8_t white[4] = {255, 255, 255, 255};
        if (!create_atlas_(white, 1, 1)) {
            shutdown();
            return false;
        }
    }

    initialized_ = true;
    std::fprintf(stderr,
                 "[sharc] ready: %u slots (%.1f MB cells), %u rays, %u lights\n",
                 config_.table_size,
                 static_cast<double>(slots * kSharcCellBytes) / (1024.0 * 1024.0),
                 config_.max_rays, config_.max_lights);
    return true;
}

void SharcGpuExecutor::begin_frame(const float3& camera_pos) {
    if (!initialized_) return;
    camera_pos_ = camera_pos;
    ++frame_index_;
    // per-frame カウンタのリセットは record() 冒頭の FillBuffer で行う
    // (counters は device-local)。 スタンプ / キー / セルは永続。
    write_params_();
}

void SharcGpuExecutor::set_counts(uint32_t ray_count, uint32_t light_count) {
    if (!initialized_) return;
    ray_count_   = std::min(ray_count, config_.max_rays);
    light_count_ = std::min(light_count, config_.max_lights);
    write_params_();
}

SharcRayGpu* SharcGpuExecutor::rays_mapped() {
    return static_cast<SharcRayGpu*>(rays_staging_.mapped);
}

SharcShadeRequestGpu* SharcGpuExecutor::shade_requests_mapped() {
    return static_cast<SharcShadeRequestGpu*>(shade_staging_.mapped);
}

SharcLightGpu* SharcGpuExecutor::lights_mapped() {
    return static_cast<SharcLightGpu*>(lights_.mapped);
}

uint32_t SharcGpuExecutor::request_count() const {
    if (!initialized_) return 0;
    // record() 末尾で counters → counters_rb_ にコピーされる (1 フレーム遅延)
    return static_cast<const uint32_t*>(counters_rb_.mapped)[1];
}

void SharcGpuExecutor::barrier_(VkCommandBuffer cmd, VkBuffer buf,
                                VkAccessFlags src, VkAccessFlags dst,
                                VkPipelineStageFlags dst_stage) {
    VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    b.srcAccessMask       = src;
    b.dstAccessMask       = dst;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer              = buf;
    b.size                = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dst_stage,
                         0, 0, nullptr, 1, &b, 0, nullptr);
}

void SharcGpuExecutor::record(VkCommandBuffer cmd) {
    if (!initialized_ || ray_count_ == 0) return;

    // ── per-frame カウンタのクリア (device-local なので GPU で行う) ──
    vkCmdFillBuffer(cmd, counters_.buf, 0, VK_WHOLE_SIZE, 0u);
    // ── D1 (CPU 一次交差) の staging → 本体転送 (dirty 時のみ) ──
    if (cpu_geometry_dirty_) {
        VkBufferCopy rc{0, 0, rays_staging_.size};
        vkCmdCopyBuffer(cmd, rays_staging_.buf, rays_.buf, 1, &rc);
        VkBufferCopy sc{0, 0, shade_staging_.size};
        vkCmdCopyBuffer(cmd, shade_staging_.buf, shade_.buf, 1, &sc);
        cpu_geometry_dirty_ = false;
    }
    {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                           VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb,
                             0, nullptr, 0, nullptr);
    }

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_,
                            0, 1, &desc_set_, 0, nullptr);

    // ── Pass 0: Hit (GPU 一次交差、 シーンアップロード済みの時のみ) ──
    if ((scene_flags_ & kSharcSceneMesh) != 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hit_.pipeline);
        vkCmdDispatch(cmd, (ray_count_ + 63u) / 64u, 1, 1);
        // hit の rays.tMax / shade 書き込み → march / resolve の読み取り
        barrier_(cmd, rays_.buf, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier_(cmd, shade_.buf, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    // パス間はグローバル memory barrier で全 SSBO を可視化する
    // (review P0-05 — 個別バッファ指定では cells/cell_pos/reservoirs の
    //  可視化が漏れていた。 パス数が少ないため全域バリアのコストは軽微)
    const auto global_barrier = [&cmd](VkPipelineStageFlags dst_stage,
                                       VkAccessFlags dst_access) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = dst_access;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             dst_stage, 0, 1, &mb, 0, nullptr, 0, nullptr);
    };

    // ── Pass 1: March (レイ並列) ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, march_.pipeline);
    vkCmdDispatch(cmd, (ray_count_ + 63u) / 64u, 1, 1);
    global_barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    // ── Pass 2: Compact (テーブル並列 — indirect 引数生成 + エビクション) ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compact_.pipeline);
    vkCmdDispatch(cmd, (config_.table_size + 255u) / 256u, 1, 1);
    global_barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                       VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                       VK_ACCESS_INDIRECT_COMMAND_READ_BIT);

    // ── Pass 3: Light / Update (1 workgroup = 1 要求セル, indirect) ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, update_.pipeline);
    vkCmdDispatchIndirect(cmd, indirect_.buf, 0);
    global_barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    // ── Pass 4: Resolve (レイ並列, キャッシュ参照のみ) ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_.pipeline);
    vkCmdDispatch(cmd, (ray_count_ + 63u) / 64u, 1, 1);

    // request_count 読み出し用に counters を host-visible へコピー
    barrier_(cmd, counters_.buf, VK_ACCESS_SHADER_WRITE_BIT,
             VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferCopy cc{0, 0, counters_rb_.size};
    vkCmdCopyBuffer(cmd, counters_.buf, counters_rb_.buf, 1, &cc);
    // 出力 (device-local) は present の fragment が読む — バリアは
    // 呼び出し側 (demo) が render pass 前に張る。
}

#endif // PICTOR_HAS_VULKAN

void SharcGpuExecutor::shutdown() {
#ifdef PICTOR_HAS_VULKAN
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        for (Pass* p : {&hit_, &march_, &compact_, &update_, &resolve_}) {
            if (p->pipeline) vkDestroyPipeline(device_, p->pipeline, nullptr);
            p->pipeline = VK_NULL_HANDLE;
        }
        if (layout_)    vkDestroyPipelineLayout(device_, layout_, nullptr);
        if (dsl_)       vkDestroyDescriptorSetLayout(device_, dsl_, nullptr);
        if (desc_pool_) vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
        layout_    = VK_NULL_HANDLE;
        dsl_       = VK_NULL_HANDLE;
        desc_pool_ = VK_NULL_HANDLE;
        desc_set_  = VK_NULL_HANDLE;
        destroy_atlas_();
        if (atlas_sampler_) vkDestroySampler(device_, atlas_sampler_, nullptr);
        atlas_sampler_ = VK_NULL_HANDLE;
        for (Buffer* b : {&params_, &keys_, &cells_, &rays_, &hits_,
                          &counters_, &requests_, &stamps_, &indirect_,
                          &lights_, &reservoirs_, &cell_pos_, &shade_,
                          &output_, &scene_nodes_, &scene_tris_,
                          &scene_tri_mats_, &scene_materials_,
                          &rays_staging_, &shade_staging_, &counters_rb_}) {
            destroy_buffer_(*b);
        }
        device_ = VK_NULL_HANDLE;
    }
    vk_ = nullptr;
#endif
    initialized_ = false;
}

} // namespace pictor
