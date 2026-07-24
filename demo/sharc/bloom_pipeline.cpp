#ifdef PICTOR_HAS_VULKAN

#include "bloom_pipeline.h"

#include "pictor/surface/vulkan_context.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace sharc_demo {

namespace {

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

constexpr VkFormat kBloomFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr float kBloomThreshold = 1.2f;   // 露出スケール後の輝度
constexpr float kBloomKnee      = 0.6f;

} // namespace

BloomPipeline::~BloomPipeline() {
    shutdown();
}

VkShaderModule BloomPipeline::load_shader_(const char* path) {
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
    if (vkCreateShaderModule(device_, &ci, nullptr, &mod) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return mod;
}

bool BloomPipeline::create_pipeline_(const char* shader_dir, const char* name,
                                     VkPipelineLayout layout,
                                     VkPipeline* out) {
    const std::string path = std::string(shader_dir) + "/" + name;
    VkShaderModule cs = load_shader_(path.c_str());
    if (cs == VK_NULL_HANDLE) {
        std::fprintf(stderr, "[bloom] shader not found: %s\n", path.c_str());
        return false;
    }
    VkComputePipelineCreateInfo ci{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr,
                0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main", nullptr};
    ci.layout = layout;
    const VkResult r = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1,
                                                &ci, nullptr, out);
    vkDestroyShaderModule(device_, cs, nullptr);
    return r == VK_SUCCESS;
}

bool BloomPipeline::initialize(pictor::VulkanContext& vk,
                               const char* shader_dir, uint32_t render_w,
                               uint32_t render_h, VkBuffer hdr_output,
                               VkDeviceSize hdr_size) {
    vk_     = &vk;
    device_ = vk.device();

    // ── mip チェーン画像 (down / up 各 kMipCount 枚、 半解像度から) ──
    uint32_t w = render_w / 2, h = render_h / 2;
    for (uint32_t m = 0; m < kMipCount; ++m) {
        mip_w_[m] = (std::max)(w, 1u);
        mip_h_[m] = (std::max)(h, 1u);
        w /= 2; h /= 2;
        for (int chain = 0; chain < 2; ++chain) {
            VkImage* img = chain ? &up_imgs_[m] : &down_imgs_[m];
            VkDeviceMemory* mem = chain ? &up_mem_[m] : &down_mem_[m];
            VkImageView* view = chain ? &up_views_[m] : &down_views_[m];
            VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ici.imageType     = VK_IMAGE_TYPE_2D;
            ici.format        = kBloomFormat;
            ici.extent        = {mip_w_[m], mip_h_[m], 1};
            ici.mipLevels     = 1;
            ici.arrayLayers   = 1;
            ici.samples       = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
            ici.usage         = VK_IMAGE_USAGE_STORAGE_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(device_, &ici, nullptr, img) != VK_SUCCESS)
                return false;
            VkMemoryRequirements mr{};
            vkGetImageMemoryRequirements(device_, *img, &mr);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize  = mr.size;
            ai.memoryTypeIndex = find_memory_type(
                vk.physical_device(), mr.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (vkAllocateMemory(device_, &ai, nullptr, mem) != VK_SUCCESS)
                return false;
            vkBindImageMemory(device_, *img, *mem, 0);
            VkImageViewCreateInfo vci{
                VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image            = *img;
            vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
            vci.format           = kBloomFormat;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(device_, &vci, nullptr, view) != VK_SUCCESS)
                return false;
        }
    }

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter    = VK_FILTER_LINEAR;
    sci.minFilter    = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &sci, nullptr, &sampler_) != VK_SUCCESS)
        return false;

    // ── descriptor set layouts ──
    //  extract: SSBO + storage image / down: sampler + storage /
    //  up: sampler×2 + storage
    const auto make_dsl = [&](std::initializer_list<VkDescriptorType> types,
                              VkDescriptorSetLayout* out) {
        VkDescriptorSetLayoutBinding b[3]{};
        uint32_t i = 0;
        for (VkDescriptorType t : types) {
            b[i] = {i, t, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            ++i;
        }
        VkDescriptorSetLayoutCreateInfo dli{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dli.bindingCount = i;
        dli.pBindings    = b;
        return vkCreateDescriptorSetLayout(device_, &dli, nullptr, out) ==
               VK_SUCCESS;
    };
    if (!make_dsl({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, &dsl_extract_) ||
        !make_dsl({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, &dsl_down_) ||
        !make_dsl({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                   VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, &dsl_up_)) {
        shutdown();
        return false;
    }

    const auto make_layout = [&](VkDescriptorSetLayout dsl,
                                 VkPipelineLayout* out) {
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                sizeof(PushParams)};
        VkPipelineLayoutCreateInfo pli{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &dsl;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &pcr;
        return vkCreatePipelineLayout(device_, &pli, nullptr, out) ==
               VK_SUCCESS;
    };
    if (!make_layout(dsl_extract_, &layout_extract_) ||
        !make_layout(dsl_down_, &layout_down_) ||
        !make_layout(dsl_up_, &layout_up_)) {
        shutdown();
        return false;
    }
    if (!create_pipeline_(shader_dir, "sharc_bloom_extract.comp.spv",
                          layout_extract_, &pipe_extract_) ||
        !create_pipeline_(shader_dir, "sharc_bloom_down.comp.spv",
                          layout_down_, &pipe_down_) ||
        !create_pipeline_(shader_dir, "sharc_bloom_up.comp.spv", layout_up_,
                          &pipe_up_)) {
        shutdown();
        return false;
    }

    // ── descriptor pool + sets ──
    const uint32_t n_sets = 1 + (kMipCount - 1) + (kMipCount - 1);
    VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, n_sets},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * kMipCount},
    };
    VkDescriptorPoolCreateInfo dpi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets       = n_sets;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes    = sizes;
    if (vkCreateDescriptorPool(device_, &dpi, nullptr, &desc_pool_) !=
        VK_SUCCESS) {
        shutdown();
        return false;
    }
    const auto alloc_set = [&](VkDescriptorSetLayout dsl,
                               VkDescriptorSet* out) {
        VkDescriptorSetAllocateInfo dai{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool     = desc_pool_;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts        = &dsl;
        return vkAllocateDescriptorSets(device_, &dai, out) == VK_SUCCESS;
    };

    // extract: SSBO → down[0]
    if (!alloc_set(dsl_extract_, &set_extract_)) {
        shutdown();
        return false;
    }
    {
        VkDescriptorBufferInfo bi{hdr_output, 0, hdr_size};
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, down_views_[0],
                                 VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[2]{};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[0].dstSet = set_extract_;
        w[0].dstBinding = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[0].pBufferInfo = &bi;
        w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[1].dstSet = set_extract_;
        w[1].dstBinding = 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo = &ii;
        vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
    }
    // down[i-1] → down[i]
    for (uint32_t m = 1; m < kMipCount; ++m) {
        if (!alloc_set(dsl_down_, &set_down_[m])) {
            shutdown();
            return false;
        }
        VkDescriptorImageInfo src{sampler_, down_views_[m - 1],
                                  VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo dst{VK_NULL_HANDLE, down_views_[m],
                                  VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[2]{};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[0].dstSet = set_down_[m];
        w[0].dstBinding = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &src;
        w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[1].dstSet = set_down_[m];
        w[1].dstBinding = 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo = &dst;
        vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
    }
    // up[i]: lower = (i == kMipCount-2 ? down[i+1] : up[i+1]) + down[i]
    for (int m = static_cast<int>(kMipCount) - 2; m >= 0; --m) {
        if (!alloc_set(dsl_up_, &set_up_[m])) {
            shutdown();
            return false;
        }
        const VkImageView lower = (m == static_cast<int>(kMipCount) - 2)
                                      ? down_views_[m + 1]
                                      : up_views_[m + 1];
        VkDescriptorImageInfo i0{sampler_, lower, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo i1{sampler_, down_views_[m],
                                 VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo i2{VK_NULL_HANDLE, up_views_[m],
                                 VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[3]{};
        const VkDescriptorImageInfo* infos[3] = {&i0, &i1, &i2};
        const VkDescriptorType types[3] = {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        for (uint32_t i = 0; i < 3; ++i) {
            w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[i].dstSet = set_up_[m];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = types[i];
            w[i].pImageInfo = infos[i];
        }
        vkUpdateDescriptorSets(device_, 3, w, 0, nullptr);
    }

    initialized_ = true;
    std::fprintf(stderr, "[bloom] ready: %ux%u x%u mips\n", mip_w_[0],
                 mip_h_[0], kMipCount);
    return true;
}

void BloomPipeline::record(VkCommandBuffer cmd, float exposure) {
    if (!initialized_) return;

    // 初回のみ全画像を UNDEFINED → GENERAL へ (以後 GENERAL 常駐)
    if (first_frame_) {
        VkImageMemoryBarrier barriers[kMipCount * 2]{};
        for (uint32_t m = 0; m < kMipCount; ++m) {
            for (int c = 0; c < 2; ++c) {
                auto& b = barriers[m * 2 + c];
                b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                b.srcAccessMask       = 0;
                b.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT |
                                        VK_ACCESS_SHADER_READ_BIT;
                b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
                b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image               = c ? up_imgs_[m] : down_imgs_[m];
                b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                         1};
            }
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, kMipCount * 2, barriers);
        first_frame_ = false;
    }

    const auto compute_barrier = [&cmd]() {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb,
                             0, nullptr, 0, nullptr);
    };

    PushParams pc{mip_w_[0] * 2, mip_h_[0] * 2, exposure, kBloomThreshold,
                  kBloomKnee};

    // extract → down[0]
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_extract_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            layout_extract_, 0, 1, &set_extract_, 0, nullptr);
    vkCmdPushConstants(cmd, layout_extract_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(pc), &pc);
    vkCmdDispatch(cmd, (mip_w_[0] + 7) / 8, (mip_h_[0] + 7) / 8, 1);

    // down チェーン
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_down_);
    for (uint32_t m = 1; m < kMipCount; ++m) {
        compute_barrier();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                layout_down_, 0, 1, &set_down_[m], 0,
                                nullptr);
        vkCmdPushConstants(cmd, layout_down_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(pc), &pc);
        vkCmdDispatch(cmd, (mip_w_[m] + 7) / 8, (mip_h_[m] + 7) / 8, 1);
    }

    // up チェーン (最下段から逆順)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_up_);
    for (int m = static_cast<int>(kMipCount) - 2; m >= 0; --m) {
        compute_barrier();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                layout_up_, 0, 1, &set_up_[m], 0, nullptr);
        vkCmdPushConstants(cmd, layout_up_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(pc), &pc);
        vkCmdDispatch(cmd, (mip_w_[m] + 7) / 8, (mip_h_[m] + 7) / 8, 1);
    }

    // up[0] → fragment 読み
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
}

void BloomPipeline::shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        for (VkPipeline p : {pipe_extract_, pipe_down_, pipe_up_}) {
            if (p) vkDestroyPipeline(device_, p, nullptr);
        }
        for (VkPipelineLayout l :
             {layout_extract_, layout_down_, layout_up_}) {
            if (l) vkDestroyPipelineLayout(device_, l, nullptr);
        }
        for (VkDescriptorSetLayout d : {dsl_extract_, dsl_down_, dsl_up_}) {
            if (d) vkDestroyDescriptorSetLayout(device_, d, nullptr);
        }
        if (desc_pool_) vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
        if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
        for (uint32_t m = 0; m < kMipCount; ++m) {
            if (down_views_[m]) vkDestroyImageView(device_, down_views_[m], nullptr);
            if (up_views_[m])   vkDestroyImageView(device_, up_views_[m], nullptr);
            if (down_imgs_[m])  vkDestroyImage(device_, down_imgs_[m], nullptr);
            if (up_imgs_[m])    vkDestroyImage(device_, up_imgs_[m], nullptr);
            if (down_mem_[m])   vkFreeMemory(device_, down_mem_[m], nullptr);
            if (up_mem_[m])     vkFreeMemory(device_, up_mem_[m], nullptr);
            down_views_[m] = up_views_[m] = VK_NULL_HANDLE;
            down_imgs_[m] = up_imgs_[m] = VK_NULL_HANDLE;
            down_mem_[m] = up_mem_[m] = VK_NULL_HANDLE;
        }
        pipe_extract_ = pipe_down_ = pipe_up_ = VK_NULL_HANDLE;
        layout_extract_ = layout_down_ = layout_up_ = VK_NULL_HANDLE;
        dsl_extract_ = dsl_down_ = dsl_up_ = VK_NULL_HANDLE;
        desc_pool_ = VK_NULL_HANDLE;
        sampler_   = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
    vk_     = nullptr;
    first_frame_ = true;
    initialized_ = false;
}

}  // namespace sharc_demo

#endif  // PICTOR_HAS_VULKAN
