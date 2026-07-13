#include "pictor/gi/gi_gpu_executor.h"

#ifdef PICTOR_HAS_VULKAN
#include "pictor/surface/vulkan_context.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace pictor {

GIGpuExecutor::~GIGpuExecutor() {
    shutdown();
}

#ifdef PICTOR_HAS_VULKAN

namespace {

/// gi_probe_sample.comp の GIProbeParams UBO と同一レイアウト (std140)。
struct GIProbeParamsGpu {
    float    grid_origin[4];
    float    grid_spacing[4];
    uint32_t grid_dimensions[4];   // xyz = probe 数, w = 総数
    uint32_t object_count;
    uint32_t probe_count;
    float    gi_intensity;
    float    max_probe_distance;
};
static_assert(sizeof(GIProbeParamsGpu) == 64,
              "GIProbeParamsGpu must match the shader UBO (64 bytes)");

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

bool GIGpuExecutor::create_buffer_(Buffer& out, VkDeviceSize size,
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
    // host-visible + coherent — CPU から毎フレーム直書きする (staging 無し。
    // GI データは小さく、 転送最適化より単純さを取る)。
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

void GIGpuExecutor::destroy_buffer_(Buffer& b) {
    if (b.mapped) vkUnmapMemory(device_, b.mem);
    if (b.buf)    vkDestroyBuffer(device_, b.buf, nullptr);
    if (b.mem)    vkFreeMemory(device_, b.mem, nullptr);
    b = Buffer{};
}

void GIGpuExecutor::write_params_() {
    GIProbeParamsGpu p{};
    p.grid_origin[0]  = config_.grid_origin.x;
    p.grid_origin[1]  = config_.grid_origin.y;
    p.grid_origin[2]  = config_.grid_origin.z;
    p.grid_spacing[0] = config_.grid_spacing.x;
    p.grid_spacing[1] = config_.grid_spacing.y;
    p.grid_spacing[2] = config_.grid_spacing.z;
    p.grid_dimensions[0] = config_.grid_x;
    p.grid_dimensions[1] = config_.grid_y;
    p.grid_dimensions[2] = config_.grid_z;
    p.grid_dimensions[3] = probe_count_;
    p.object_count       = object_count_;
    p.probe_count        = probe_count_;
    p.gi_intensity       = config_.gi_intensity;
    p.max_probe_distance = config_.max_probe_distance;
    std::memcpy(params_.mapped, &p, sizeof(p));
}

bool GIGpuExecutor::initialize(VulkanContext& vk, const std::string& shader_dir,
                               uint32_t max_objects,
                               const GIProbeConfig& config) {
    vk_     = &vk;
    device_ = vk.device();
    config_ = config;
    max_objects_ = std::max(max_objects, 1u);
    probe_count_ = std::max(config.grid_x, 1u) * std::max(config.grid_y, 1u)
                 * std::max(config.grid_z, 1u);

    // ── buffers ──
    const VkDeviceSize sh_stride = 9 * 4 * sizeof(float);
    if (!create_buffer_(params_, sizeof(GIProbeParamsGpu),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
        !create_buffer_(transforms_, VkDeviceSize{max_objects_} * sizeof(float4x4),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !create_buffer_(visibility_, VkDeviceSize{max_objects_} * sizeof(uint32_t),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !create_buffer_(probe_sh_, VkDeviceSize{probe_count_} * sh_stride,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !create_buffer_(object_irradiance_,
                        VkDeviceSize{max_objects_} * sh_stride,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
        std::fprintf(stderr, "[gi-gpu] buffer creation failed\n");
        shutdown();
        return false;
    }
    write_params_();

    // ── descriptor set layout (UBO + SSBO×4) ──
    VkDescriptorSetLayoutBinding bindings[5]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    for (uint32_t i = 1; i < 5; ++i) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo dli{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 5;
    dli.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(device_, &dli, nullptr, &dsl_) != VK_SUCCESS) {
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

    // ── compute pipeline ──
    const std::string spv = shader_dir + "/gi_probe_sample.comp.spv";
    VkShaderModule cs = load_shader_module(device_, spv);
    if (cs == VK_NULL_HANDLE) {
        // 必須前提の欠落 — 黙って縮退しない (RULE_CODE §7.1 fail-fast)。
        std::fprintf(stderr, "[gi-gpu] compute shader not found: %s\n",
                     spv.c_str());
        shutdown();
        return false;
    }
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_COMPUTE_BIT, cs, "main", nullptr};
    ci.layout = layout_;
    const VkResult pr =
        vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr,
                                 &pipeline_);
    vkDestroyShaderModule(device_, cs, nullptr);
    if (pr != VK_SUCCESS) {
        std::fprintf(stderr, "[gi-gpu] compute pipeline creation failed\n");
        shutdown();
        return false;
    }

    // ── descriptor pool + set (バッファは固定なので 1 回書けば終わり) ──
    VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
    };
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets       = 1;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes    = sizes;
    if (vkCreateDescriptorPool(device_, &dpi, nullptr, &desc_pool_) != VK_SUCCESS) {
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

    const VkDescriptorBufferInfo infos[5] = {
        {params_.buf,            0, params_.size},
        {transforms_.buf,        0, transforms_.size},
        {visibility_.buf,        0, visibility_.size},
        {probe_sh_.buf,          0, probe_sh_.size},
        {object_irradiance_.buf, 0, object_irradiance_.size},
    };
    VkWriteDescriptorSet writes[5]{};
    for (uint32_t i = 0; i < 5; ++i) {
        writes[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet          = desc_set_;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = (i == 0) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                             : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo     = &infos[i];
    }
    vkUpdateDescriptorSets(device_, 5, writes, 0, nullptr);

    initialized_ = true;
    std::fprintf(stderr, "[gi-gpu] ready: %u probes, max %u objects\n",
                 probe_count_, max_objects_);
    return true;
}

void GIGpuExecutor::set_probe_config(const GIProbeConfig& config) {
    if (!initialized_) return;
    // grid 寸法は init 時に確定済み — 値のみ反映する (寸法変更は再 init)。
    config_.gi_intensity       = config.gi_intensity;
    config_.max_probe_distance = config.max_probe_distance;
    config_.grid_origin        = config.grid_origin;
    config_.grid_spacing       = config.grid_spacing;
    write_params_();
}

void GIGpuExecutor::upload_probe_sh(const float* sh_data, uint32_t probe_count) {
    if (!initialized_ || sh_data == nullptr) return;
    const size_t stride = 9 * 4 * sizeof(float);
    const size_t count  = std::min<size_t>(probe_count, probe_count_);
    std::memcpy(probe_sh_.mapped, sh_data, count * stride);
}

void GIGpuExecutor::update_objects(const float4x4* transforms, uint32_t count,
                                   const uint8_t* visibility) {
    if (!initialized_ || transforms == nullptr) return;
    object_count_ = std::min(count, max_objects_);
    std::memcpy(transforms_.mapped, transforms,
                size_t{object_count_} * sizeof(float4x4));
    auto* vis = static_cast<uint32_t*>(visibility_.mapped);
    for (uint32_t i = 0; i < object_count_; ++i) {
        vis[i] = (visibility == nullptr || visibility[i] != 0) ? 1u : 0u;
    }
    write_params_();   // object_count を UBO へ反映
}

void GIGpuExecutor::record(VkCommandBuffer cmd) {
    if (!initialized_ || object_count_ == 0) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_,
                            0, 1, &desc_set_, 0, nullptr);
    const uint32_t groups = (object_count_ + 255u) / 256u;
    vkCmdDispatch(cmd, groups, 1, 1);

    // compute write → 以降のシェーダ read (per-object irradiance を
    // vertex / fragment が読む)。
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer              = object_irradiance_.buf;
    barrier.size                = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
}

#endif // PICTOR_HAS_VULKAN

void GIGpuExecutor::shutdown() {
#ifdef PICTOR_HAS_VULKAN
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (pipeline_)  vkDestroyPipeline(device_, pipeline_, nullptr);
        if (layout_)    vkDestroyPipelineLayout(device_, layout_, nullptr);
        if (dsl_)       vkDestroyDescriptorSetLayout(device_, dsl_, nullptr);
        if (desc_pool_) vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
        pipeline_  = VK_NULL_HANDLE;
        layout_    = VK_NULL_HANDLE;
        dsl_       = VK_NULL_HANDLE;
        desc_pool_ = VK_NULL_HANDLE;
        desc_set_  = VK_NULL_HANDLE;
        destroy_buffer_(params_);
        destroy_buffer_(transforms_);
        destroy_buffer_(visibility_);
        destroy_buffer_(probe_sh_);
        destroy_buffer_(object_irradiance_);
        device_ = VK_NULL_HANDLE;
    }
    vk_ = nullptr;
#endif
    initialized_ = false;
}

} // namespace pictor
