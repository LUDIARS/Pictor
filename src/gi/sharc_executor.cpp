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

constexpr uint32_t kBindingCount = 14;

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
                                      VkBufferUsageFlags usage) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size        = size;
    bi.usage       = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bi, nullptr, &out.buf) != VK_SUCCESS)
        return false;

    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(device_, out.buf, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = mr.size;
    // host-visible + coherent — CPU 直書き / 直読み (GIGpuExecutor と同方針)。
    ai.memoryTypeIndex = find_memory_type(
        vk_->physical_device(), mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device_, &ai, nullptr, &out.mem) != VK_SUCCESS)
        return false;
    vkBindBufferMemory(device_, out.buf, out.mem, 0);
    if (vkMapMemory(device_, out.mem, 0, size, 0, &out.mapped) != VK_SUCCESS)
        return false;
    out.size = size;
    std::memset(out.mapped, 0, static_cast<size_t>(size));
    return true;
}

void SharcGpuExecutor::destroy_buffer_(Buffer& b) {
    if (b.mapped) vkUnmapMemory(device_, b.mem);
    if (b.buf)    vkDestroyBuffer(device_, b.buf, nullptr);
    if (b.mem)    vkFreeMemory(device_, b.mem, nullptr);
    b = Buffer{};
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
    std::memcpy(params_.mapped, &p, sizeof(p));
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
    const VkDeviceSize slots = config_.table_size;
    const auto ssbo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (!create_buffer_(params_, sizeof(SharcParamsGpu),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
        !create_buffer_(keys_, slots * sizeof(uint32_t), ssbo) ||
        !create_buffer_(cells_, slots * kSharcCellBytes, ssbo) ||
        !create_buffer_(rays_, VkDeviceSize{config_.max_rays} *
                        sizeof(SharcRayGpu), ssbo) ||
        !create_buffer_(hits_, VkDeviceSize{config_.max_hits} *
                        kSharcHitRecordBytes, ssbo) ||
        !create_buffer_(counters_, 4 * sizeof(uint32_t), ssbo) ||
        !create_buffer_(requests_, slots * sizeof(uint32_t), ssbo) ||
        !create_buffer_(stamps_, slots * sizeof(uint32_t), ssbo) ||
        !create_buffer_(indirect_, 3 * sizeof(uint32_t),
                        ssbo | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) ||
        !create_buffer_(lights_, VkDeviceSize{config_.max_lights} *
                        sizeof(SharcLightGpu), ssbo) ||
        !create_buffer_(reservoirs_, slots * kSharcReservoirUints *
                        sizeof(uint32_t), ssbo) ||
        !create_buffer_(cell_pos_, slots * 2 * sizeof(uint32_t), ssbo) ||
        !create_buffer_(shade_, VkDeviceSize{config_.max_rays} *
                        sizeof(SharcShadeRequestGpu), ssbo) ||
        !create_buffer_(output_, VkDeviceSize{config_.max_rays} *
                        4 * sizeof(float), ssbo)) {
        std::fprintf(stderr, "[sharc] buffer creation failed\n");
        shutdown();
        return false;
    }
    write_params_();

    // ── descriptor set layout (UBO + SSBO×13) ──
    VkDescriptorSetLayoutBinding bindings[kBindingCount]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    for (uint32_t i = 1; i < kBindingCount; ++i) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
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

    // ── compute pipelines (4 パス) ──
    if (!create_pass_(march_,   shader_dir + "/sharc_march.comp.spv") ||
        !create_pass_(compact_, shader_dir + "/sharc_compact.comp.spv") ||
        !create_pass_(update_,  shader_dir + "/sharc_update.comp.spv") ||
        !create_pass_(resolve_, shader_dir + "/sharc_resolve.comp.spv")) {
        shutdown();
        return false;
    }

    // ── descriptor pool + set ──
    VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kBindingCount - 1},
    };
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets       = 1;
    dpi.poolSizeCount = 2;
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

    const Buffer* ordered[kBindingCount] = {
        &params_, &keys_, &cells_, &rays_, &hits_, &counters_, &requests_,
        &stamps_, &indirect_, &lights_, &reservoirs_, &cell_pos_, &shade_,
        &output_,
    };
    VkDescriptorBufferInfo infos[kBindingCount]{};
    VkWriteDescriptorSet writes[kBindingCount]{};
    for (uint32_t i = 0; i < kBindingCount; ++i) {
        infos[i] = {ordered[i]->buf, 0, ordered[i]->size};
        writes[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet          = desc_set_;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = (i == 0) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                             : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo     = &infos[i];
    }
    vkUpdateDescriptorSets(device_, kBindingCount, writes, 0, nullptr);

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
    // per-frame カウンタ (hit / request) をリセット。 スタンプ / キー /
    // セルは永続 (SHaRC 方式の時間蓄積)。
    std::memset(counters_.mapped, 0, counters_.size);
    write_params_();
}

void SharcGpuExecutor::set_counts(uint32_t ray_count, uint32_t light_count) {
    if (!initialized_) return;
    ray_count_   = std::min(ray_count, config_.max_rays);
    light_count_ = std::min(light_count, config_.max_lights);
    write_params_();
}

SharcRayGpu* SharcGpuExecutor::rays_mapped() {
    return static_cast<SharcRayGpu*>(rays_.mapped);
}

SharcShadeRequestGpu* SharcGpuExecutor::shade_requests_mapped() {
    return static_cast<SharcShadeRequestGpu*>(shade_.mapped);
}

SharcLightGpu* SharcGpuExecutor::lights_mapped() {
    return static_cast<SharcLightGpu*>(lights_.mapped);
}

const float* SharcGpuExecutor::output_mapped() const {
    return static_cast<const float*>(output_.mapped);
}

uint32_t SharcGpuExecutor::request_count() const {
    if (!initialized_) return 0;
    return static_cast<const uint32_t*>(counters_.mapped)[1];
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

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_,
                            0, 1, &desc_set_, 0, nullptr);

    // ── Pass 1: March (レイ並列) ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, march_.pipeline);
    vkCmdDispatch(cmd, (ray_count_ + 63u) / 64u, 1, 1);

    // march の書き込み (キー / 要求リスト / カウンタ) → compact の読み取り
    barrier_(cmd, counters_.buf, VK_ACCESS_SHADER_WRITE_BIT,
             VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    barrier_(cmd, keys_.buf, VK_ACCESS_SHADER_WRITE_BIT,
             VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // ── Pass 2: Compact (テーブル並列 — indirect 引数生成 + エビクション) ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compact_.pipeline);
    vkCmdDispatch(cmd, (config_.table_size + 255u) / 256u, 1, 1);

    // compact の indirect 書き込み → indirect dispatch 読み取り
    barrier_(cmd, indirect_.buf, VK_ACCESS_SHADER_WRITE_BIT,
             VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
    barrier_(cmd, requests_.buf, VK_ACCESS_SHADER_WRITE_BIT,
             VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // ── Pass 3: Light / Update (1 workgroup = 1 要求セル, indirect) ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, update_.pipeline);
    vkCmdDispatchIndirect(cmd, indirect_.buf, 0);

    // update のセル書き込み → resolve の読み取り
    barrier_(cmd, cells_.buf, VK_ACCESS_SHADER_WRITE_BIT,
             VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // ── Pass 4: Resolve (レイ並列, キャッシュ参照のみ) ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_.pipeline);
    vkCmdDispatch(cmd, (ray_count_ + 63u) / 64u, 1, 1);

    // resolve の出力 → ホスト読み取り
    barrier_(cmd, output_.buf, VK_ACCESS_SHADER_WRITE_BIT,
             VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT);
}

#endif // PICTOR_HAS_VULKAN

void SharcGpuExecutor::shutdown() {
#ifdef PICTOR_HAS_VULKAN
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        for (Pass* p : {&march_, &compact_, &update_, &resolve_}) {
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
        for (Buffer* b : {&params_, &keys_, &cells_, &rays_, &hits_,
                          &counters_, &requests_, &stamps_, &indirect_,
                          &lights_, &reservoirs_, &cell_pos_, &shade_,
                          &output_}) {
            destroy_buffer_(*b);
        }
        device_ = VK_NULL_HANDLE;
    }
    vk_ = nullptr;
#endif
    initialized_ = false;
}

} // namespace pictor
