#include "pictor/gi/sharc_scene_acceleration.h"

#ifdef PICTOR_HAS_VULKAN
#include "pictor/surface/vulkan_context.h"
#endif

#include <algorithm>
#include <cstdio>
#include <map>
#include <tuple>

namespace pictor {

#ifdef PICTOR_HAS_VULKAN

namespace {

constexpr VkDeviceSize kScratchAlignment = 256;  // above every device's minAccelerationStructureScratchOffsetAlignment

VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

uint32_t memory_type(VkPhysicalDevice physical, uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((filter & (1u << i)) != 0 && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    return UINT32_MAX;
}

}  // namespace

SharcSceneAcceleration::~SharcSceneAcceleration() { shutdown(); }

bool SharcSceneAcceleration::load_functions_() {
    const auto get = [this](const char* name) { return vkGetDeviceProcAddr(device_, name); };
    get_build_sizes_ = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(get("vkGetAccelerationStructureBuildSizesKHR"));
    create_ = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(get("vkCreateAccelerationStructureKHR"));
    destroy_ = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(get("vkDestroyAccelerationStructureKHR"));
    cmd_build_ = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(get("vkCmdBuildAccelerationStructuresKHR"));
    get_structure_address_ =
        reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(get("vkGetAccelerationStructureDeviceAddressKHR"));
    if (get_build_sizes_ == nullptr || create_ == nullptr || destroy_ == nullptr || cmd_build_ == nullptr ||
        get_structure_address_ == nullptr) {
        std::fprintf(stderr, "[sharc] the device has no acceleration-structure functions "
                             "(create the context with VulkanConfig::require_ray_query)\n");
        return false;
    }
    return true;
}

bool SharcSceneAcceleration::create_buffer_(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible, Buffer& out) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = std::max<VkDeviceSize>(size, 16);
    info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &info, nullptr, &out.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements needs{};
    vkGetBufferMemoryRequirements(device_, out.buffer, &needs);
    VkMemoryAllocateFlagsInfo address{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    address.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.pNext = &address;
    alloc.allocationSize = needs.size;
    alloc.memoryTypeIndex = memory_type(vk_->physical_device(), needs.memoryTypeBits,
                                        host_visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                     : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &alloc, nullptr, &out.memory) != VK_SUCCESS ||
        vkBindBufferMemory(device_, out.buffer, out.memory, 0) != VK_SUCCESS) {
        return false;
    }
    if (host_visible && vkMapMemory(device_, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) != VK_SUCCESS) return false;
    out.size = info.size;
    return true;
}

void SharcSceneAcceleration::destroy_buffer_(Buffer& b) {
    if (b.mapped != nullptr) vkUnmapMemory(device_, b.memory);
    if (b.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device_, b.buffer, nullptr);
    if (b.memory != VK_NULL_HANDLE) vkFreeMemory(device_, b.memory, nullptr);
    b = Buffer{};
}

VkDeviceAddress SharcSceneAcceleration::address_of_(VkBuffer buffer) const {
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(device_, &info);
}

bool SharcSceneAcceleration::create_structure_(VkAccelerationStructureTypeKHR type, VkDeviceSize size, Structure& out) {
    if (!create_buffer_(size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, out.storage)) return false;
    VkAccelerationStructureCreateInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    info.buffer = out.storage.buffer;
    info.size = size;
    info.type = type;
    return create_(device_, &info, nullptr, &out.handle) == VK_SUCCESS;
}

void SharcSceneAcceleration::destroy_structure_(Structure& s) {
    if (s.handle != VK_NULL_HANDLE) destroy_(device_, s.handle, nullptr);
    s.handle = VK_NULL_HANDLE;
    destroy_buffer_(s.storage);
}

bool SharcSceneAcceleration::build(VulkanContext& vk, const SharcGeometryBuffers& geometry,
                                   const std::vector<uint32_t>& indices, const std::vector<SharcPlacement>& placements) {
    shutdown();
    vk_ = &vk;
    device_ = vk.device();
    if (!vk.has_ray_query()) {
        std::fprintf(stderr, "[sharc] ray queries are off (VulkanConfig::require_ray_query)\n");
        return false;
    }
    if (!load_functions_()) return false;
    if (placements.empty() || geometry.vertices == VK_NULL_HANDLE || geometry.indices == VK_NULL_HANDLE ||
        geometry.vertex_stride < 12) {
        std::fprintf(stderr, "[sharc] no geometry to trace against\n");
        return false;
    }
    const VkDeviceAddress vertices = address_of_(geometry.vertices);
    const VkDeviceAddress index_base = address_of_(geometry.indices);

    // One bottom level per distinct range; placements of the same mesh share it.
    using Range = std::tuple<uint32_t, uint32_t, int32_t>;  // first index, count, vertex offset
    std::map<Range, uint32_t> range_of;
    std::vector<Range> ranges;
    std::vector<uint32_t> placement_range(placements.size());
    for (size_t p = 0; p < placements.size(); ++p) {
        const Range key{placements[p].first_index, placements[p].index_count, placements[p].vertex_offset};
        const auto [it, added] = range_of.emplace(key, static_cast<uint32_t>(ranges.size()));
        if (added) ranges.push_back(key);
        placement_range[p] = it->second;
    }

    std::vector<VkAccelerationStructureGeometryKHR> geometries(ranges.size());
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> builds(ranges.size());
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> build_ranges(ranges.size());
    std::vector<VkDeviceSize> scratch_offsets(ranges.size());
    VkDeviceSize scratch_total = 0;
    bottom_.resize(ranges.size());
    for (size_t r = 0; r < ranges.size(); ++r) {
        const auto [first, count, offset] = ranges[r];
        uint32_t max_vertex = 0;
        for (uint32_t i = first; i < first + count && i < indices.size(); ++i) max_vertex = std::max(max_vertex, indices[i]);
        VkAccelerationStructureGeometryKHR& g = geometries[r];
        g = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& t = g.geometry.triangles;
        t = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        t.vertexData.deviceAddress = vertices + static_cast<VkDeviceAddress>(offset) * geometry.vertex_stride;
        t.vertexStride = geometry.vertex_stride;
        t.maxVertex = max_vertex;
        t.indexType = VK_INDEX_TYPE_UINT32;
        t.indexData.deviceAddress = index_base + static_cast<VkDeviceAddress>(first) * sizeof(uint32_t);

        VkAccelerationStructureBuildGeometryInfoKHR& b = builds[r];
        b = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        b.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        b.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        b.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        b.geometryCount = 1;
        b.pGeometries = &g;
        const uint32_t triangles = count / 3;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        get_build_sizes_(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &b, &triangles, &sizes);
        if (!create_structure_(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sizes.accelerationStructureSize, bottom_[r])) {
            std::fprintf(stderr, "[sharc] cannot create a bottom-level acceleration structure\n");
            return false;
        }
        b.dstAccelerationStructure = bottom_[r].handle;
        scratch_offsets[r] = scratch_total;
        scratch_total += align_up(sizes.buildScratchSize, kScratchAlignment);
        build_ranges[r] = {triangles, 0, 0, 0};
    }

    // Top level: every placement with its transform (column-major in, row-major 3x4 out).
    Buffer instances;
    if (!create_buffer_(sizeof(VkAccelerationStructureInstanceKHR) * placements.size(),
                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, instances) ||
        !create_buffer_(sizeof(SharcRayInstanceGpu) * placements.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                        instance_table_)) {
        destroy_buffer_(instances);
        return false;
    }
    auto* placed = static_cast<VkAccelerationStructureInstanceKHR*>(instances.mapped);
    auto* table = static_cast<SharcRayInstanceGpu*>(instance_table_.mapped);
    for (size_t p = 0; p < placements.size(); ++p) {
        VkAccelerationStructureInstanceKHR inst{};
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 4; ++col) inst.transform.matrix[row][col] = placements[p].transform[col * 4 + row];
        }
        inst.instanceCustomIndex = static_cast<uint32_t>(p);
        inst.mask = 0xFF;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        VkAccelerationStructureDeviceAddressInfoKHR where{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        where.accelerationStructure = bottom_[placement_range[p]].handle;
        inst.accelerationStructureReference = get_structure_address_(device_, &where);
        placed[p] = inst;
        table[p] = {placements[p].first_index, placements[p].vertex_offset, placements[p].surface, 0u};
    }
    VkAccelerationStructureGeometryKHR top_geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    top_geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    top_geometry.geometry.instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    top_geometry.geometry.instances.data.deviceAddress = address_of_(instances.buffer);
    VkAccelerationStructureBuildGeometryInfoKHR top_build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    top_build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    top_build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    top_build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    top_build.geometryCount = 1;
    top_build.pGeometries = &top_geometry;
    const auto instance_count = static_cast<uint32_t>(placements.size());
    VkAccelerationStructureBuildSizesInfoKHR top_sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    get_build_sizes_(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &top_build, &instance_count, &top_sizes);
    if (!create_structure_(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, top_sizes.accelerationStructureSize, top_)) {
        destroy_buffer_(instances);
        return false;
    }
    top_build.dstAccelerationStructure = top_.handle;

    // Scratch for every bottom level at once (each at its own offset), then the top level's.
    Buffer scratch;
    const VkDeviceSize scratch_bytes = std::max(scratch_total, align_up(top_sizes.buildScratchSize, kScratchAlignment));
    if (!create_buffer_(scratch_bytes + kScratchAlignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, scratch)) {
        destroy_buffer_(instances);
        return false;
    }
    const VkDeviceAddress scratch_base = align_up(address_of_(scratch.buffer), kScratchAlignment);
    for (size_t r = 0; r < builds.size(); ++r) builds[r].scratchData.deviceAddress = scratch_base + scratch_offsets[r];
    top_build.scratchData.deviceAddress = scratch_base;

    VkCommandBuffer cmd = vk.begin_single_time_commands();
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> range_pointers(build_ranges.size());
    for (size_t r = 0; r < build_ranges.size(); ++r) range_pointers[r] = &build_ranges[r];
    cmd_build_(cmd, static_cast<uint32_t>(builds.size()), builds.data(), range_pointers.data());
    VkMemoryBarrier built{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    built.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    built.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &built, 0, nullptr, 0, nullptr);
    const VkAccelerationStructureBuildRangeInfoKHR top_range{instance_count, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* top_range_pointer = &top_range;
    cmd_build_(cmd, 1, &top_build, &top_range_pointer);
    vk.end_single_time_commands(cmd);  // waits for the queue: scratch and instances can go

    destroy_buffer_(scratch);
    destroy_buffer_(instances);
    std::printf("[sharc] acceleration structures: %zu meshes, %zu placements\n", ranges.size(), placements.size());
    return true;
}

#endif  // PICTOR_HAS_VULKAN

void SharcSceneAcceleration::shutdown() {
#ifdef PICTOR_HAS_VULKAN
    if (device_ == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(device_);
    for (Structure& s : bottom_) destroy_structure_(s);
    bottom_.clear();
    destroy_structure_(top_);
    destroy_buffer_(instance_table_);
    device_ = VK_NULL_HANDLE;
#endif
}

}  // namespace pictor
