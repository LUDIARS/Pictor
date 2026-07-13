#include "pictor/gi/gi_ssao_compute.h"

#ifdef PICTOR_HAS_VULKAN
#include "pictor/surface/vulkan_context.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace pictor {

GISsaoCompute::~GISsaoCompute() {
    shutdown();
}

#ifdef PICTOR_HAS_VULKAN

namespace {

/// ssao_gen.comp の SSAOParams UBO と同一レイアウト (std140)。
struct SsaoParamsGpu {
    float    projection[16];
    float    inv_projection[16];
    uint32_t screen_size[2];
    float    radius;
    float    bias;
    float    intensity;
    uint32_t sample_count;
    float    falloff_start;
    float    falloff_end;
};
static_assert(sizeof(SsaoParamsGpu) == 160,
              "SsaoParamsGpu must match the shader UBO (160 bytes)");

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

bool create_host_buffer(VkDevice device, VkPhysicalDevice phys,
                        VkDeviceSize size, VkBufferUsageFlags usage,
                        VkBuffer& buf, VkDeviceMemory& mem, void*& mapped) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size        = size;
    bi.usage       = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bi, nullptr, &buf) != VK_SUCCESS) return false;

    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(device, buf, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = find_memory_type(
        phys, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device, &ai, nullptr, &mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(device, buf, mem, 0);
    if (vkMapMemory(device, mem, 0, size, 0, &mapped) != VK_SUCCESS)
        return false;
    std::memset(mapped, 0, static_cast<size_t>(size));
    return true;
}

} // namespace

void GISsaoCompute::write_params_() {
    // 行列部は update_camera が直接書く — ここでは値パラメータのみ更新。
    auto* p = static_cast<SsaoParamsGpu*>(params_mapped_);
    p->screen_size[0] = width_;
    p->screen_size[1] = height_;
    p->radius         = config_.radius;
    p->bias           = config_.bias;
    p->intensity      = config_.intensity;
    p->sample_count   = std::min(config_.sample_count, kMaxKernelSamples);
    p->falloff_start  = config_.falloff_start;
    p->falloff_end    = config_.falloff_end;
}

bool GISsaoCompute::create_ao_image_(uint32_t width, uint32_t height) {
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType     = VK_IMAGE_TYPE_2D;
    ic.format        = VK_FORMAT_R8_UNORM;
    ic.extent        = {width, height, 1};
    ic.mipLevels     = 1;
    ic.arrayLayers   = 1;
    ic.samples       = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ic.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ic.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ic, nullptr, &ao_image_) != VK_SUCCESS)
        return false;

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(device_, ao_image_, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = find_memory_type(vk_->physical_device(),
                                          mr.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &ai, nullptr, &ao_memory_) != VK_SUCCESS)
        return false;
    vkBindImageMemory(device_, ao_image_, ao_memory_, 0);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = ao_image_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = VK_FORMAT_R8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &vi, nullptr, &ao_view_) != VK_SUCCESS)
        return false;

    ao_image_fresh_ = true;
    return true;
}

void GISsaoCompute::destroy_ao_image_() {
    if (ao_view_)   vkDestroyImageView(device_, ao_view_, nullptr);
    if (ao_image_)  vkDestroyImage(device_, ao_image_, nullptr);
    if (ao_memory_) vkFreeMemory(device_, ao_memory_, nullptr);
    ao_view_   = VK_NULL_HANDLE;
    ao_image_  = VK_NULL_HANDLE;
    ao_memory_ = VK_NULL_HANDLE;
}

bool GISsaoCompute::initialize(VulkanContext& vk, const std::string& shader_dir,
                               uint32_t width, uint32_t height,
                               const SSAOConfig& config) {
    vk_     = &vk;
    device_ = vk.device();
    config_ = config;
    width_  = width;
    height_ = height;

    if (!create_ao_image_(width, height)) {
        std::fprintf(stderr, "[gi-ssao] AO image creation failed\n");
        shutdown();
        return false;
    }

    // ── samplers (AO 出力読み用 LINEAR / 深度入力用 NEAREST) ──
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &si, nullptr, &ao_sampler_) != VK_SUCCESS) {
        shutdown();
        return false;
    }
    si.magFilter = VK_FILTER_NEAREST;   // D32 の LINEAR は保証されない
    si.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(device_, &si, nullptr, &depth_sampler_) != VK_SUCCESS) {
        shutdown();
        return false;
    }

    // ── buffers ──
    if (!create_host_buffer(device_, vk.physical_device(),
                            sizeof(SsaoParamsGpu),
                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            params_buf_, params_mem_, params_mapped_) ||
        !create_host_buffer(device_, vk.physical_device(),
                            VkDeviceSize{kMaxKernelSamples} * 4 * sizeof(float),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            kernel_buf_, kernel_mem_, kernel_mapped_)) {
        std::fprintf(stderr, "[gi-ssao] buffer creation failed\n");
        shutdown();
        return false;
    }
    // 行列は単位行列で初期化 (update_camera 前の dispatch でも壊れない)。
    auto* p = static_cast<SsaoParamsGpu*>(params_mapped_);
    for (int i = 0; i < 4; ++i) {
        p->projection[i * 4 + i]     = 1.0f;
        p->inv_projection[i * 4 + i] = 1.0f;
    }
    write_params_();
    generate_ssao_kernel(std::min(config_.sample_count, kMaxKernelSamples),
                         static_cast<float*>(kernel_mapped_));

    // ── descriptor set layout / pipeline layout ──
    VkDescriptorSetLayoutBinding bindings[4]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dli{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 4;
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
    const std::string spv = shader_dir + "/ssao_gen.comp.spv";
    VkShaderModule cs = load_shader_module(device_, spv);
    if (cs == VK_NULL_HANDLE) {
        // 必須前提の欠落 — 黙って縮退しない (RULE_CODE §7.1 fail-fast)。
        std::fprintf(stderr, "[gi-ssao] compute shader not found: %s\n",
                     spv.c_str());
        shutdown();
        return false;
    }
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_COMPUTE_BIT, cs, "main", nullptr};
    ci.layout = layout_;
    const VkResult pr = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1,
                                                 &ci, nullptr, &pipeline_);
    vkDestroyShaderModule(device_, cs, nullptr);
    if (pr != VK_SUCCESS) {
        std::fprintf(stderr, "[gi-ssao] compute pipeline creation failed\n");
        shutdown();
        return false;
    }

    // ── descriptor pool + set ──
    VkDescriptorPoolSize sizes[4] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
    };
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets       = 1;
    dpi.poolSizeCount = 4;
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

    // バッファ / AO image の descriptor は今書く。 深度は set_depth_input()。
    const VkDescriptorBufferInfo buf_infos[2] = {
        {params_buf_, 0, sizeof(SsaoParamsGpu)},
        {kernel_buf_, 0, VkDeviceSize{kMaxKernelSamples} * 4 * sizeof(float)},
    };
    VkDescriptorImageInfo ao_info{VK_NULL_HANDLE, ao_view_,
                                  VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[3]{};
    for (int i = 0; i < 2; ++i) {
        writes[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet          = desc_set_;
        writes[i].dstBinding      = static_cast<uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = (i == 0) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                             : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo     = &buf_infos[i];
    }
    writes[2] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[2].dstSet          = desc_set_;
    writes[2].dstBinding      = 3;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo      = &ao_info;
    vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);

    initialized_ = true;
    std::fprintf(stderr, "[gi-ssao] ready: %ux%u, %u samples\n",
                 width, height, std::min(config_.sample_count,
                                         kMaxKernelSamples));
    return true;
}

void GISsaoCompute::set_depth_input(VkImageView depth_view,
                                    VkImageLayout layout) {
    if (!initialized_ || depth_view == VK_NULL_HANDLE) return;
    VkDescriptorImageInfo info{depth_sampler_, depth_view, layout};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet          = desc_set_;
    write.dstBinding      = 2;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    depth_bound_ = true;
}

void GISsaoCompute::update_camera(const float4x4& projection,
                                  const float4x4& inv_projection) {
    if (!initialized_) return;
    auto* p = static_cast<SsaoParamsGpu*>(params_mapped_);
    std::memcpy(p->projection, projection.m, sizeof(p->projection));
    std::memcpy(p->inv_projection, inv_projection.m, sizeof(p->inv_projection));
}

void GISsaoCompute::set_config(const SSAOConfig& config) {
    if (!initialized_) return;
    const bool kernel_dirty = config.sample_count != config_.sample_count;
    config_ = config;
    write_params_();
    if (kernel_dirty) {
        generate_ssao_kernel(std::min(config_.sample_count, kMaxKernelSamples),
                             static_cast<float*>(kernel_mapped_));
    }
}

void GISsaoCompute::record(VkCommandBuffer cmd) {
    if (!initialized_ || !depth_bound_) return;

    // AO image → GENERAL (書込み用)。 初回は UNDEFINED から。
    VkImageMemoryBarrier to_general{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_general.srcAccessMask = ao_image_fresh_ ? 0 : VK_ACCESS_SHADER_READ_BIT;
    to_general.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    to_general.oldLayout     = ao_image_fresh_
        ? VK_IMAGE_LAYOUT_UNDEFINED
        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_general.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.image = ao_image_;
    to_general.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
                         ao_image_fresh_ ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                         : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_general);
    ao_image_fresh_ = false;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_,
                            0, 1, &desc_set_, 0, nullptr);
    vkCmdDispatch(cmd, (width_ + 15u) / 16u, (height_ + 15u) / 16u, 1);

    // compute write → fragment read (マテリアルが AO をサンプル)。
    VkImageMemoryBarrier to_read = to_general;
    to_read.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_read.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    to_read.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_read);
}

bool GISsaoCompute::resize(uint32_t width, uint32_t height) {
    if (!initialized_) return false;
    vkDeviceWaitIdle(device_);
    destroy_ao_image_();
    width_  = width;
    height_ = height;
    if (!create_ao_image_(width, height)) {
        initialized_ = false;
        return false;
    }
    write_params_();
    // AO image の storage descriptor を新ビューへ更新。
    VkDescriptorImageInfo ao_info{VK_NULL_HANDLE, ao_view_,
                                  VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet          = desc_set_;
    write.dstBinding      = 3;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo      = &ao_info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return true;
}

#endif // PICTOR_HAS_VULKAN

void GISsaoCompute::shutdown() {
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

        if (params_mapped_) vkUnmapMemory(device_, params_mem_);
        if (params_buf_)    vkDestroyBuffer(device_, params_buf_, nullptr);
        if (params_mem_)    vkFreeMemory(device_, params_mem_, nullptr);
        if (kernel_mapped_) vkUnmapMemory(device_, kernel_mem_);
        if (kernel_buf_)    vkDestroyBuffer(device_, kernel_buf_, nullptr);
        if (kernel_mem_)    vkFreeMemory(device_, kernel_mem_, nullptr);
        params_buf_ = kernel_buf_ = VK_NULL_HANDLE;
        params_mem_ = kernel_mem_ = VK_NULL_HANDLE;
        params_mapped_ = kernel_mapped_ = nullptr;

        if (ao_sampler_)    vkDestroySampler(device_, ao_sampler_, nullptr);
        if (depth_sampler_) vkDestroySampler(device_, depth_sampler_, nullptr);
        ao_sampler_ = depth_sampler_ = VK_NULL_HANDLE;
        destroy_ao_image_();
        device_ = VK_NULL_HANDLE;
    }
    vk_ = nullptr;
#endif
    initialized_ = false;
}

} // namespace pictor
