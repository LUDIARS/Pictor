#include "pictor/gi/sharc_irradiance_cache.h"

#ifdef PICTOR_HAS_VULKAN
#include "pictor/surface/vulkan_context.h"
#endif

#include "pictor/gi/sharc_types.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace pictor {

#ifdef PICTOR_HAS_VULKAN

namespace {

// Bindings (shaders/sharc/sharc_common.glsl 0-19, sharc_irradiance*.glsl 30+).
constexpr uint32_t kParams = 0, kKeys = 1, kCells = 2, kAccum = 4;
constexpr uint32_t kFrame = 30, kScene = 31, kInstances = 32, kVertices = 33, kIndices = 34, kSurfaces = 35,
                   kLights = 36, kSunShadow = 37, kDepth = 38, kNormal = 39, kOutput = 40;

uint32_t memory_type(VkPhysicalDevice physical, uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((filter & (1u << i)) != 0 && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    return UINT32_MAX;
}

VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

}  // namespace

SharcIrradianceCache::~SharcIrradianceCache() { shutdown(); }

bool SharcIrradianceCache::create_buffer_(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible, Buffer& out) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &info, nullptr, &out.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements needs{};
    vkGetBufferMemoryRequirements(device_, out.buffer, &needs);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = needs.size;
    alloc.memoryTypeIndex = memory_type(vk_->physical_device(), needs.memoryTypeBits,
                                        host_visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                     : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &alloc, nullptr, &out.memory) != VK_SUCCESS ||
        vkBindBufferMemory(device_, out.buffer, out.memory, 0) != VK_SUCCESS) {
        return false;
    }
    if (host_visible && vkMapMemory(device_, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) != VK_SUCCESS) return false;
    out.size = size;
    return true;
}

void SharcIrradianceCache::destroy_buffer_(Buffer& b) {
    if (b.mapped != nullptr) vkUnmapMemory(device_, b.memory);
    if (b.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device_, b.buffer, nullptr);
    if (b.memory != VK_NULL_HANDLE) vkFreeMemory(device_, b.memory, nullptr);
    b = Buffer{};
}

bool SharcIrradianceCache::create_pipeline_(const std::string& path, VkPipeline& out) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "[sharc] shader not found: %s\n", path.c_str());
        return false;
    }
    const auto size = static_cast<size_t>(file.tellg());
    if (size == 0 || size % 4 != 0) return false;
    std::vector<uint32_t> code(size / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size));
    VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = size;
    module_info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &module_info, nullptr, &module) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module,
                  "main", nullptr};
    info.layout = layout_;
    const VkResult result = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &out);
    vkDestroyShaderModule(device_, module, nullptr);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "[sharc] compute pipeline creation failed: %s\n", path.c_str());
        return false;
    }
    return true;
}

bool SharcIrradianceCache::initialize(VulkanContext& vk, const std::string& shader_dir,
                                      const SharcIrradianceConfig& config) {
    shutdown();
    vk_ = &vk;
    device_ = vk.device();
    config_ = config;
    if (!vk.has_ray_query()) {
        std::fprintf(stderr, "[sharc] the irradiance cache needs ray queries (VulkanConfig::require_ray_query)\n");
        return false;
    }
    if (config.table_size == 0 || (config.table_size & (config.table_size - 1)) != 0 || config.level_count == 0 ||
        config.level_count * 6 > 255 || config.update_block == 0) {
        std::fprintf(stderr, "[sharc] bad irradiance cache configuration\n");
        return false;
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(vk.physical_device(), &properties);
    const VkDeviceSize align = properties.limits.minUniformBufferOffsetAlignment;
    slots_ = std::max(1u, vk.frames_in_flight());
    params_stride_ = align_up(sizeof(SharcParamsGpu), align);
    frame_stride_ = align_up(sizeof(SharcIrradianceFrameGpu), align);
    const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!create_buffer_(params_stride_ * slots_, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, params_) ||
        !create_buffer_(frame_stride_ * slots_, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, frame_) ||
        !create_buffer_(sizeof(uint32_t) * config.table_size, storage, false, keys_) ||
        !create_buffer_(static_cast<VkDeviceSize>(kSharcCellBytes) * config.table_size, storage, false, cells_) ||
        !create_buffer_(sizeof(uint32_t) * kSharcAccumUints * config.table_size, storage, false, accum_)) {
        std::fprintf(stderr, "[sharc] cannot allocate the irradiance cache (%u slots)\n", config.table_size);
        shutdown();
        return false;
    }
    // An empty table: every key 0, every cell and sum 0.
    VkCommandBuffer cmd = vk.begin_single_time_commands();
    for (const Buffer* b : {&keys_, &cells_, &accum_}) vkCmdFillBuffer(cmd, b->buffer, 0, VK_WHOLE_SIZE, 0);
    vk.end_single_time_commands(cmd);

    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &sampler, nullptr, &nearest_) != VK_SUCCESS) {
        shutdown();
        return false;
    }

    const VkShaderStageFlags cs = VK_SHADER_STAGE_COMPUTE_BIT;
    const std::array<VkDescriptorSetLayoutBinding, 15> bindings{{
        {kParams, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, cs, nullptr},
        {kKeys, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, nullptr},
        {kCells, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, nullptr},
        {kAccum, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, nullptr},
        {kFrame, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, cs, nullptr},
        {kScene, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, cs, nullptr},
        {kInstances, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, nullptr},
        {kVertices, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, nullptr},
        {kIndices, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, nullptr},
        {kSurfaces, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, nullptr},
        {kLights, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, nullptr},
        {kSunShadow, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, cs, nullptr},
        {kDepth, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, cs, nullptr},
        {kNormal, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, cs, nullptr},
        {kOutput, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, cs, nullptr},
    }};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = static_cast<uint32_t>(bindings.size());
    set_info.pBindings = bindings.data();
    const VkPushConstantRange push{cs, 0, sizeof(vertex_layout_)};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    const std::array<VkDescriptorPoolSize, 6> sizes{{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2},
                                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8},
                                                     {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
                                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3},
                                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
                                                     {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pool.pPoolSizes = sizes.data();
    if (vkCreateDescriptorSetLayout(device_, &set_info, nullptr, &set_layout_) != VK_SUCCESS ||
        vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout_) != VK_SUCCESS ||
        vkCreateDescriptorPool(device_, &pool, nullptr, &pool_) != VK_SUCCESS) {
        shutdown();
        return false;
    }
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &set_layout_;
    if (vkAllocateDescriptorSets(device_, &alloc, &set_) != VK_SUCCESS ||
        !create_pipeline_(shader_dir + "/sharc_irradiance_update.comp.spv", update_) ||
        !create_pipeline_(shader_dir + "/sharc_irradiance_resolve.comp.spv", resolve_) ||
        !create_pipeline_(shader_dir + "/sharc_irradiance_query.comp.spv", query_)) {
        shutdown();
        return false;
    }
    write_buffer_(kParams, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, params_.buffer, 0, sizeof(SharcParamsGpu));
    write_buffer_(kFrame, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, frame_.buffer, 0, sizeof(SharcIrradianceFrameGpu));
    write_buffer_(kKeys, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, keys_.buffer, 0, keys_.size);
    write_buffer_(kCells, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cells_.buffer, 0, cells_.size);
    write_buffer_(kAccum, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, accum_.buffer, 0, accum_.size);
    initialized_ = true;
    return true;
}

void SharcIrradianceCache::write_buffer_(uint32_t binding, VkDescriptorType type, VkBuffer buffer, VkDeviceSize offset,
                                         VkDeviceSize range) {
    const VkDescriptorBufferInfo info{buffer, offset, range};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set_;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void SharcIrradianceCache::write_image_(uint32_t binding, VkDescriptorType type, VkImageView view, VkImageLayout layout) {
    const VkDescriptorImageInfo info{type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? VK_NULL_HANDLE : nearest_, view, layout};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set_;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void SharcIrradianceCache::set_scene(const SharcIrradianceScene& scene) {
    VkWriteDescriptorSetAccelerationStructureKHR structure{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    structure.accelerationStructureCount = 1;
    structure.pAccelerationStructures = &scene.top_level;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.pNext = &structure;
    write.dstSet = set_;
    write.dstBinding = kScene;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    write_buffer_(kInstances, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene.instances, 0, scene.instances_bytes);
    write_buffer_(kVertices, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene.vertices, 0, scene.vertices_bytes);
    write_buffer_(kIndices, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene.indices, 0, scene.indices_bytes);
    write_buffer_(kSurfaces, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene.surfaces, 0, scene.surfaces_bytes);
    vertex_layout_[0] = scene.vertex_stride_floats;
    vertex_layout_[1] = scene.position_offset_floats;
    vertex_layout_[2] = scene.normal_offset_floats;
    has_scene_ = scene.top_level != VK_NULL_HANDLE && scene.vertex_stride_floats != 0;
}

void SharcIrradianceCache::set_lights(VkBuffer lights, VkDeviceSize bytes) {
    write_buffer_(kLights, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, lights, 0, bytes);
    has_lights_ = lights != VK_NULL_HANDLE;
}

void SharcIrradianceCache::set_sun_shadow(VkImageView shadow, VkImageLayout layout) {
    write_image_(kSunShadow, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, shadow, layout);
    has_shadow_ = shadow != VK_NULL_HANDLE;
}

bool SharcIrradianceCache::create_output_(VkExtent2D extent) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &info, nullptr, &output_) != VK_SUCCESS) return false;
    VkMemoryRequirements needs{};
    vkGetImageMemoryRequirements(device_, output_, &needs);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = needs.size;
    alloc.memoryTypeIndex = memory_type(vk_->physical_device(), needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &alloc, nullptr, &output_memory_) != VK_SUCCESS ||
        vkBindImageMemory(device_, output_, output_memory_, 0) != VK_SUCCESS) {
        return false;
    }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = output_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = info.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &view, nullptr, &output_view_) != VK_SUCCESS) return false;
    output_extent_ = extent;
    output_fresh_ = true;
    return true;
}

void SharcIrradianceCache::destroy_output_() {
    if (output_view_ != VK_NULL_HANDLE) vkDestroyImageView(device_, output_view_, nullptr);
    if (output_ != VK_NULL_HANDLE) vkDestroyImage(device_, output_, nullptr);
    if (output_memory_ != VK_NULL_HANDLE) vkFreeMemory(device_, output_memory_, nullptr);
    output_view_ = VK_NULL_HANDLE;
    output_ = VK_NULL_HANDLE;
    output_memory_ = VK_NULL_HANDLE;
    output_extent_ = {};
}

bool SharcIrradianceCache::set_gbuffer(VkImageView depth, VkImageLayout depth_layout, VkImageView normal,
                                       VkExtent2D extent) {
    if (extent.width != output_extent_.width || extent.height != output_extent_.height) {
        vkDeviceWaitIdle(device_);
        destroy_output_();
        if (!create_output_(extent)) {
            std::fprintf(stderr, "[sharc] cannot create the irradiance image (%ux%u)\n", extent.width, extent.height);
            has_gbuffer_ = false;
            return false;
        }
    }
    write_image_(kDepth, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, depth, depth_layout);
    write_image_(kNormal, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, normal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    write_image_(kOutput, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, output_view_, VK_IMAGE_LAYOUT_GENERAL);
    has_gbuffer_ = true;
    return true;
}

bool SharcIrradianceCache::ready() const {
    return initialized_ && has_scene_ && has_lights_ && has_shadow_ && has_gbuffer_;
}

void SharcIrradianceCache::compute_barrier_(VkCommandBuffer cmd) const {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0,
                         nullptr, 0, nullptr);
}

bool SharcIrradianceCache::record(VkCommandBuffer cmd, const SharcIrradianceFrameGpu& frame) {
    if (!ready()) return false;
    const uint32_t slot = frame_counter_++ % slots_;
    SharcParamsGpu params{};
    params.camera_pos[0] = frame.camera[0];
    params.camera_pos[1] = frame.camera[1];
    params.camera_pos[2] = frame.camera[2];
    params.base_cell_size = config_.base_cell_size;
    params.level_count = config_.level_count;
    params.table_size = config_.table_size;
    params.frame_index = frame.counts[1];
    params.level_bias = config_.level_bias;
    params.stale_frames = config_.stale_frames;
    std::memcpy(static_cast<char*>(params_.mapped) + params_stride_ * slot, &params, sizeof(params));
    SharcIrradianceFrameGpu f = frame;
    f.counts[2] = config_.update_block;
    std::memcpy(static_cast<char*>(frame_.mapped) + frame_stride_ * slot, &f, sizeof(f));
    const uint32_t offsets[2] = {static_cast<uint32_t>(params_stride_ * slot), static_cast<uint32_t>(frame_stride_ * slot)};

    // The irradiance image: from what fragment shaders read last frame to compute writes.
    VkImageMemoryBarrier to_write{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_write.oldLayout = output_fresh_ ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_write.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_write.srcAccessMask = output_fresh_ ? 0 : VK_ACCESS_SHADER_READ_BIT;
    to_write.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    to_write.srcQueueFamilyIndex = to_write.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_write.image = output_;
    to_write.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // The G-buffer was just drawn: its writes before these reads; last frame's cache reads before these writes.
    VkMemoryBarrier inputs{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    inputs.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    inputs.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &inputs, 0, nullptr, 1, &to_write);
    output_fresh_ = false;

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set_, 2, offsets);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vertex_layout_), vertex_layout_);
    const uint32_t width = static_cast<uint32_t>(frame.screen[0]);
    const uint32_t height = static_cast<uint32_t>(frame.screen[1]);
    const uint32_t block = config_.update_block;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, update_);
    vkCmdDispatch(cmd, ((width + block - 1) / block + 7) / 8, ((height + block - 1) / block + 7) / 8, 1);
    compute_barrier_(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_);
    vkCmdDispatch(cmd, (config_.table_size + 63) / 64, 1, 1);
    compute_barrier_(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, query_);
    vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);

    VkImageMemoryBarrier to_read = to_write;
    to_read.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_read);
    return true;
}

#endif  // PICTOR_HAS_VULKAN

void SharcIrradianceCache::shutdown() {
#ifdef PICTOR_HAS_VULKAN
    if (device_ == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(device_);
    for (VkPipeline* p : {&update_, &resolve_, &query_}) {
        if (*p != VK_NULL_HANDLE) vkDestroyPipeline(device_, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    if (pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (set_layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (nearest_ != VK_NULL_HANDLE) vkDestroySampler(device_, nearest_, nullptr);
    pool_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    nearest_ = VK_NULL_HANDLE;
    set_ = VK_NULL_HANDLE;
    destroy_output_();
    for (Buffer* b : {&params_, &frame_, &keys_, &cells_, &accum_}) destroy_buffer_(*b);
    has_scene_ = has_lights_ = has_shadow_ = has_gbuffer_ = false;
    device_ = VK_NULL_HANDLE;
#endif
    initialized_ = false;
}

}  // namespace pictor
